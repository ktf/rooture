#include "rooture.h"
#include <cstdarg>
#include <ctime>

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------

bool g_debug = false;

static int             g_pipe_fds[2];
static replxx::Replxx* g_rx = nullptr;
static TSParser*       g_ts_parser = nullptr;

// Saved for exec-based reload
static char**      g_argv = nullptr;

// MCP mode
static bool        g_mcp_mode = false;
static bool        g_mcp_reload = false;  // true when restarted via execv reload
static int         g_mcp_reply_fds[2];
static FILE*       g_mcp_out = stdout;  // real JSON-RPC output stream (saved before fd-1 redirect)
static bool        g_capturing = false;
static std::string g_capture_buf;
static std::string g_screenshot_dir;  // directory to archive screenshots (git repo)

// ---------------------------------------------------------------------------
// screenshot_archive — copy PNG to screenshot dir and git-commit it
// ---------------------------------------------------------------------------

// Returns the destination path on success, empty string on failure.
static std::string screenshot_archive(const std::string& src_png,
                                      const std::string& slug,
                                      const std::string& motivation,
                                      const std::string& description)
{
  if (g_screenshot_dir.empty()) return "";

  // Build timestamped filename: YYYYMMDD_HHMMSS_<slug>.png
  char ts[32];
  std::time_t now = std::time(nullptr);
  std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now));
  std::string filename = std::string(ts) + "_" + slug + ".png";
  std::string dest = g_screenshot_dir + "/" + filename;

  // Copy file
  std::ifstream in(src_png, std::ios::binary);
  std::ofstream out(dest, std::ios::binary);
  if (!in || !out) return "";
  out << in.rdbuf();
  in.close(); out.close();

  // git add + commit
  auto shell_escape = [](const std::string& s) {
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
  };
  std::string msg = motivation;
  if (!description.empty()) msg += "\n\n" + description;
  std::string cmd =
    "git -C " + shell_escape(g_screenshot_dir) +
    " add " + shell_escape(filename) +
    " && git -C " + shell_escape(g_screenshot_dir) +
    " commit -m " + shell_escape(msg) +
    " >/dev/null 2>&1";
  std::system(cmd.c_str());

  // Return the ID: filename stem without extension (e.g. "20260507_123456_canvas_gauss")
  std::string stem = filename.substr(0, filename.size() - 4); // strip ".png"
  return stem;
}

// ---------------------------------------------------------------------------
// rut_print — writes to replxx output pipe (or stdout in non-REPL mode)
// ---------------------------------------------------------------------------

void rut_print(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  if (g_capturing) {
    char buf[4096]; vsnprintf(buf, sizeof(buf), fmt, ap);
    g_capture_buf += buf;
  } else if (g_mcp_mode) {
    // discard: stdout is reserved for JSON-RPC in MCP mode
  } else if (g_rx) {
    char buf[4096]; vsnprintf(buf, sizeof(buf), fmt, ap);
    g_rx->print("%s", buf);
  } else {
    vprintf(fmt, ap);
  }
  va_end(ap);
}

// ---------------------------------------------------------------------------
// lenv_add_builtins — top-level coordinator
// ---------------------------------------------------------------------------

void lenv_add_builtins(lenv* e) {
  lenv_add_builtins_lang(e);
  lenv_add_builtins_root(e);
  lenv_add_builtins_jitfn(e);
  lenv_add_builtins_column(e);
  lenv_add_builtins_net(e);
}

//----- Pipe-based input handler -----------------------------------------------

class PipeHandler : public TFileHandler {
  lenv* fEnv;
public:
  PipeHandler(lenv* e) : TFileHandler(g_pipe_fds[0], 1), fEnv(e) {}

  Bool_t Notify() override {
    uint32_t len = 0;
    if (read(g_pipe_fds[0], &len, sizeof(len)) != sizeof(len)) return kTRUE;
    if (len == 0) { gApplication->Terminate(0); return kTRUE; }

    std::string expr(len, '\0');
    size_t got = 0;
    while (got < len) {
      ssize_t n = read(g_pipe_fds[0], expr.data() + got, len - got);
      if (n <= 0) break;
      got += n;
    }

    mpc_result_t r;
    const char* src = g_mcp_mode ? "<mcp>" : "<stdin>";

    int saved_stderr_fd = -1;
    int stderr_pipe[2] = {-1, -1};
    if (g_mcp_mode) {
      g_capturing = true;
      g_capture_buf.clear();
      // Redirect stderr to capture Cling diagnostics
      saved_stderr_fd = dup(STDERR_FILENO);
      pipe(stderr_pipe);
      dup2(stderr_pipe[1], STDERR_FILENO);
      close(stderr_pipe[1]);
      stderr_pipe[1] = -1;
    }

    if (mpc_parse(src, expr.c_str(), Lispy, &r)) {
      if (g_debug) mpc_ast_print((mpc_ast_t*)r.output);
      lval* prog = lval_read((mpc_ast_t*)r.output);
      mpc_ast_delete((mpc_ast_t*)r.output);
      /* Evaluate top-level expressions in sequence, print last result. */
      lval* x = lval_sexpr();
      while (prog->count) {
        lval_del(x);
        x = lval_eval(fEnv, lval_pop(prog, 0));
        if (x->type == LVAL_ERR) break;
      }
      lval_del(prog);
      lval_println(x);
      lval_del(x);
    } else {
      char* err_str = mpc_err_string(r.error);
      rut_print("%s\n", err_str);
      free(err_str);
      mpc_err_delete(r.error);
    }

    if (g_mcp_mode) {
      g_capturing = false;

      // Restore stderr and collect any Cling diagnostics
      if (saved_stderr_fd >= 0) {
        fflush(stderr);
        dup2(saved_stderr_fd, STDERR_FILENO);
        close(saved_stderr_fd);
        // Drain the pipe (non-blocking)
        fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
        char sbuf[4096];
        ssize_t sn;
        std::string diag;
        while ((sn = read(stderr_pipe[0], sbuf, sizeof(sbuf))) > 0)
          diag.append(sbuf, sn);
        close(stderr_pipe[0]);
        if (!diag.empty()) {
          if (!g_capture_buf.empty()) g_capture_buf += "\n";
          g_capture_buf += diag;
        }
      }

      uint32_t rlen = (uint32_t)g_capture_buf.size();
      write(g_mcp_reply_fds[1], &rlen, sizeof(rlen));
      if (rlen > 0) write(g_mcp_reply_fds[1], g_capture_buf.data(), rlen);
    }

    TIter next(gROOT->GetListOfCanvases());
    TVirtualPad* c;
    while ((c = (TVirtualPad*)next())) c->Update();
    // Flush GUI widget redraws (e.g. TGLabel::SetText → NeedRedraw).
    // Update(2) calls TGClient::DoRedraw() via TGCocoa's friend access;
    // Update(1) flushes the resulting Cocoa command buffer to the screen.
    // (Mirrors what TMacOSXSystem::DispatchOneEvent does after Cocoa events.)
    if (gVirtualX) { gVirtualX->Update(2); gVirtualX->Update(1); }
    TInterpreter::Instance()->EndOfLineAction();
    return kTRUE;
  }

  Bool_t ReadNotify() override { return Notify(); }
};

//----- Syntax highlighter -----------------------------------------------------

static const std::vector<std::string> kKeywords = {
  "def", "=", "\\", "if", "fun", "do", "let",
  "->", ".", "::", "new", "load", "error", "print", "println"
};

static void highlight_rooture(std::string const& input,
                               replxx::Replxx::colors_t& colors) {
  using Color = replxx::Replxx::Color;
  if (!g_ts_parser) return;

  TSTree* tree = ts_parser_parse_string(g_ts_parser, nullptr,
                                        input.c_str(), (uint32_t)input.size());
  if (!tree) return;

  TSNode root = ts_tree_root_node(tree);
  TSTreeCursor cursor = ts_tree_cursor_new(root);

  auto color_range = [&](uint32_t start, uint32_t end, Color c) {
    for (uint32_t i = start; i < end && i < colors.size(); ++i)
      colors[i] = c;
  };

  std::function<void()> walk = [&]() {
    TSNode node = ts_tree_cursor_current_node(&cursor);
    if (ts_node_child_count(node) == 0) {
      uint32_t start = ts_node_start_byte(node);
      uint32_t end   = ts_node_end_byte(node);
      std::string_view text(input.data() + start, end - start);
      const char* type = ts_node_type(node);

      if (strcmp(type, "dot_method") == 0) {
        color_range(start, end, Color::BRIGHTGREEN);
      } else if (strcmp(type, "named_ref") == 0) {
        color_range(start, end, Color::MAGENTA);
      } else if (strcmp(type, "number") == 0 || strcmp(type, "float") == 0) {
        color_range(start, end, Color::CYAN);
      } else if (strcmp(type, "string") == 0) {
        color_range(start, end, Color::YELLOW);
      } else if (strcmp(type, "comment") == 0) {
        color_range(start, end, Color::BROWN);
      } else if (strcmp(type, "(") == 0 || strcmp(type, ")") == 0 ||
                 strcmp(type, "{") == 0 || strcmp(type, "}") == 0) {
        color_range(start, end, Color::WHITE);
      } else if (strcmp(type, "symbol") == 0) {
        if (text.find("::") != std::string_view::npos) {
          color_range(start, end, Color::BRIGHTBLUE);
        } else {
          bool is_kw = std::find(kKeywords.begin(), kKeywords.end(), text)
                       != kKeywords.end();
          color_range(start, end, is_kw ? Color::BRIGHTGREEN : Color::LIGHTGRAY);
        }
      }
    }

    if (ts_tree_cursor_goto_first_child(&cursor)) {
      walk();
      while (ts_tree_cursor_goto_next_sibling(&cursor)) walk();
      ts_tree_cursor_goto_parent(&cursor);
    }
  };

  walk();
  ts_tree_cursor_delete(&cursor);
  ts_tree_delete(tree);
}

//----- Completion callback ----------------------------------------------------

static replxx::Replxx::completions_t complete_rooture(
    std::string const& input, int& context_len)
{
  replxx::Replxx::completions_t completions;

  // Find the start of the current token
  int pos = (int)input.size() - 1;
  while (pos >= 0 && input[pos] != ' ' && input[pos] != '(' && input[pos] != ')' &&
         input[pos] != '{' && input[pos] != '}')
    --pos;
  std::string word = input.substr(pos + 1);

  if (word.empty() || word[0] != '@') return completions;

  std::string prefix = word.substr(1); // strip '@'
  context_len = (int)word.size();

  // Collect matching names as strings first, then deduplicate
  std::vector<std::string> names;
  auto add_from_list = [&](TCollection* lst) {
    if (!lst) return;
    TIter next(lst);
    TObject* obj;
    while ((obj = next())) {
      const char* raw = obj->GetName();
      if (!raw || raw[0] == '\0' || strcmp(raw, obj->ClassName()) == 0) continue;
      std::string name(raw);
      if (prefix.empty() || name.substr(0, prefix.size()) == prefix)
        names.push_back("@" + name);
    }
  };

  add_from_list(gROOT->GetListOfCanvases());
  add_from_list(gROOT->GetListOfFiles());
  add_from_list(gROOT->GetListOfSpecials());
  if (gDirectory) add_from_list(gDirectory->GetList());

  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  for (auto const& n : names) completions.push_back(n);

  return completions;
}

//----- Input thread -----------------------------------------------------------

static void input_thread_fn() {
  replxx::Replxx rx;
  rx.set_max_history_size(1000);
  g_rx = &rx;

  g_ts_parser = ts_parser_new();
  ts_parser_set_language(g_ts_parser, tree_sitter_rooture());
  rx.set_highlighter_callback(highlight_rooture);
  rx.set_completion_callback(complete_rooture);
  rx.set_word_break_characters(" \t\n\r(){}\"");

  rx.print("ROOTure 0.1.0\nPress Ctrl+c to interrupt, Ctrl+d to exit\n\n");

  // Load history
  const char* home = getenv("HOME");
  std::string hist_file;
  if (home) { hist_file = std::string(home) + "/.rooture_history"; rx.history_load(hist_file); }

  auto send_expr = [](const std::string& expr) {
    uint32_t len = (uint32_t)expr.size();
    write(g_pipe_fds[1], &len, sizeof(len));
    write(g_pipe_fds[1], expr.data(), len);
  };

  std::string accumulated;
  int open_depth = 0;

  while (true) {
    const char* prompt = accumulated.empty() ? "ROOTure> " : "      .. ";
    const char* input = rx.input(prompt);
    if (!input) {
      // EOF (Ctrl+D)
      uint32_t eof = 0;
      write(g_pipe_fds[1], &eof, sizeof(eof));
      break;
    }

    std::string line(input);

    // Count parens/braces to support multi-line input
    for (char ch : line) {
      if (ch == '(' || ch == '{') open_depth++;
      else if (ch == ')' || ch == '}') open_depth--;
    }

    if (accumulated.empty()) accumulated = line;
    else accumulated += "\n" + line;

    if (open_depth <= 0) {
      open_depth = 0;
      if (!accumulated.empty() && accumulated.find_first_not_of(" \t\n\r") != std::string::npos) {
        rx.history_add(accumulated);
        send_expr(accumulated);
      }
      accumulated.clear();
    }
  }

  if (!hist_file.empty()) rx.history_save(hist_file);

  ts_parser_delete(g_ts_parser);
  g_ts_parser = nullptr;
  g_rx = nullptr;
}

//----- MCP stdio server -------------------------------------------------------

static std::string base64_encode(const std::vector<uint8_t>& data) {
  static const char* b64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i+1] << 8) | data[i+2];
    out += b64[(v >> 18) & 63]; out += b64[(v >> 12) & 63];
    out += b64[(v >>  6) & 63]; out += b64[v & 63];
  }
  if (i < data.size()) {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < data.size()) v |= (uint32_t)data[i+1] << 8;
    out += b64[(v >> 18) & 63];
    out += b64[(v >> 12) & 63];
    out += (i + 1 < data.size()) ? b64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

static void mcp_thread_fn() {
  using json = nlohmann::json;

  // Helper: send eval expression to main thread, block until result arrives.
  auto eval_expr = [](const std::string& expr) -> std::string {
    uint32_t len = (uint32_t)expr.size();
    write(g_pipe_fds[1], &len, sizeof(len));
    write(g_pipe_fds[1], expr.data(), len);
    uint32_t rlen = 0;
    if (read(g_mcp_reply_fds[0], &rlen, sizeof(rlen)) != sizeof(rlen)) return "";
    std::string result(rlen, '\0');
    size_t got = 0;
    while (got < rlen) {
      ssize_t n = read(g_mcp_reply_fds[0], result.data() + got, rlen - got);
      if (n <= 0) break;
      got += n;
    }
    return result;
  };

  // Helper: write one JSON-RPC response line to the saved MCP output stream.
  // Use error_handler_t::replace so invalid UTF-8 in error messages never
  // throws and kills the process.
  auto send_resp = [](const json& resp) {
    std::string s = resp.dump(-1, ' ', false,
                              json::error_handler_t::replace) + "\n";
    fwrite(s.c_str(), 1, s.size(), g_mcp_out);
    fflush(g_mcp_out);
  };

  json tools = json::array({
    {{"name", "eval"},
     {"description", "Evaluate a rooture expression in ROOT (Lisp-like syntax)."},
     {"inputSchema", {{"type","object"},
       {"properties", {{"expr", {{"type","string"},{"description","Expression to evaluate"}}}}},
       {"required", {"expr"}}}}},
    {{"name", "list_symbols"},
     {"description", "List all user-defined symbols in the rooture environment."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
    {{"name", "list_canvases"},
     {"description", "List names of all open ROOT TCanvas objects."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
    {{"name", "get_canvas"},
     {"description",
       "Return a ROOT TCanvas as a PNG image. "
       "The image is archived to the screenshot directory (if configured) and committed to git. "
       "REQUIRED WORKFLOW: after calling this tool you MUST examine the returned image carefully, "
       "verify whether your hypothesis stated in 'description' matches what you actually see, "
       "and then call amend_screenshot with the returned id and your detailed observations. "
       "Skipping amend_screenshot leaves the analysis incomplete and undocumented."},
     {"inputSchema", {{"type","object"},
       {"properties", {
         {"name", {{"type","string"},{"description","Canvas name"}}},
         {"motivation", {{"type","string"},{"description","One-line reason for taking this screenshot (used as git commit subject)."}}},
         {"description", {{"type","string"},{"description","What you expect to see — your hypothesis. This is committed with the image so it can later be compared against the actual observations recorded via amend_screenshot."}}}
       }},
       {"required", {"name","motivation","description"}}}}},
    {{"name", "get_window"},
     {"description",
       "Capture a ROOT GUI window (TGFrame / TGMainFrame) as a PNG image. "
       "The image is archived to the screenshot directory (if configured) and committed to git. "
       "REQUIRED WORKFLOW: after calling this tool you MUST examine the returned image carefully, "
       "verify whether your hypothesis stated in 'description' matches what you actually see, "
       "and then call amend_screenshot with the returned id and your detailed observations. "
       "Skipping amend_screenshot leaves the analysis incomplete and undocumented."},
     {"inputSchema", {{"type","object"},
       {"properties", {
         {"symbol", {{"type","string"},{"description","Rooture symbol name of the TGFrame variable"}}},
         {"motivation", {{"type","string"},{"description","One-line reason for taking this screenshot (used as git commit subject)."}}},
         {"description", {{"type","string"},{"description","What you expect to see — your hypothesis. This is committed with the image so it can later be compared against the actual observations recorded via amend_screenshot."}}}
       }},
       {"required", {"symbol","motivation","description"}}}}},
    {{"name", "amend_screenshot"},
     {"description",
       "Record your observations about a screenshot taken with get_canvas or get_window. "
       "You MUST call this after every get_canvas/get_window call, once you have examined the image. "
       "Write what you actually see: confirm or refute the hypothesis, note anomalies, unexpected features, "
       "quality issues, or any follow-up actions needed. "
       "This commits a sidecar .md file alongside the PNG in the screenshot repository, "
       "creating a permanent record that pairs the expected result with the actual observation."},
     {"inputSchema", {{"type","object"},
       {"properties", {
         {"id",           {{"type","string"},{"description","Screenshot id returned by get_canvas or get_window."}}},
         {"observations", {{"type","string"},{"description","What you actually see in the image. Compare against the hypothesis, note any discrepancies, quality issues, or follow-up actions."}}}
       }},
       {"required", {"id","observations"}}}}},
    {{"name", "list_annotations"},
     {"description", "List all symbol annotations set via (annotate sym \"text\"). Returns {name annotation} pairs describing user-defined customisation points."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
    {{"name", "reload"},
     {"description", "Restart the rooture process with a freshly built binary. The new process automatically signals the client to refresh its tool list — no manual /mcp reconnect needed."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
  });

  // Hot-reload: the previous process signalled us via env var.  Send a log
  // notification so the user knows the session was reset, then notify the
  // client to refresh its tool list — no manual /mcp reconnect needed.
  if (g_mcp_reload) {
    send_resp({{"jsonrpc","2.0"},{"method","notifications/message"},{"params",{
      {"level","warning"},
      {"data","rooture session reset: all state (loaded scripts, defined symbols, open canvases) has been lost."}
    }}});
    send_resp({{"jsonrpc","2.0"},{"method","notifications/tools/list_changed"}});
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    json req;
    try { req = json::parse(line); } catch (...) { continue; }

    json id   = req.contains("id") ? req["id"] : json(nullptr);
    std::string method = req.value("method", "");

    if (method == "initialize") {
      send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
        {"protocolVersion","2024-11-05"},
        {"capabilities",{{"tools",json::object()}}},
        {"serverInfo",{{"name","rooture"},{"version","0.1.0 (" __DATE__ " " __TIME__ ")"}}}
      }}});
    } else if (method == "initialized") {
      /* notification — no response */
    } else if (method == "tools/list") {
      send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{{"tools",tools}}}});
    } else if (method == "tools/call") {
      std::string tool = req["params"].value("name","");
      json args = req["params"].value("arguments", json::object());

      if (tool == "eval") {
        std::string expr = args.value("expr","");
        std::string result = eval_expr(expr);
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "list_symbols") {
        std::string result = eval_expr("(symbols)");
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "list_annotations") {
        std::string result = eval_expr("(annotations)");
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "list_canvases") {
        std::string result = eval_expr("(canvases)");
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "get_canvas") {
        std::string name        = args.value("name","");
        std::string motivation  = args.value("motivation","");
        std::string description = args.value("description","");
        std::string tmp = "/tmp/rooture_canvas_" + name + ".png";
        // Escape name/path for the rooture string literal
        eval_expr("(save-png \"" + name + "\" \"" + tmp + "\")");
        // Read PNG file and base64-encode
        std::ifstream f(tmp, std::ios::binary);
        std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        if (png.empty()) {
          send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
            {"code",-32000},{"message","Failed to save canvas '"+name+"' as PNG"}
          }}});
        } else {
          std::string scr_id = screenshot_archive(tmp, "canvas_" + name, motivation, description);
          if (scr_id.empty()) scr_id = "(none — screenshot-dir not configured)";
          std::string id_msg = "id: " + scr_id + "\nExamine the image carefully and call amend_screenshot with this id and your observations.";
          std::string b64 = base64_encode(png);
          send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
            {"content", json::array({
              {{"type","text"},{"text",id_msg}},
              {{"type","image"},{"data",b64},{"mimeType","image/png"}}
            })}
          }}});
        }
        std::remove(tmp.c_str());

      } else if (tool == "get_window") {
        std::string symbol      = args.value("symbol","");
        std::string motivation  = args.value("motivation","");
        std::string description = args.value("description","");
        std::string tmp = "/tmp/rooture_window_" + symbol + ".png";
        // Flush pending window-manager events so MapRaised takes effect
        // before we capture the screen contents.
        eval_expr("(process-line \"for(int _i=0;_i<5;_i++) gSystem->ProcessEvents();\")");
        eval_expr("(save-window " + symbol + " \"" + tmp + "\")");
        std::ifstream wf(tmp, std::ios::binary);
        std::vector<uint8_t> png((std::istreambuf_iterator<char>(wf)),
                                  std::istreambuf_iterator<char>());
        if (png.empty()) {
          send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
            {"code",-32000},{"message","Failed to capture window '"+symbol+"' as PNG"}
          }}});
        } else {
          std::string scr_id = screenshot_archive(tmp, "window_" + symbol, motivation, description);
          if (scr_id.empty()) scr_id = "(none — screenshot-dir not configured)";
          std::string id_msg = "id: " + scr_id + "\nExamine the image carefully and call amend_screenshot with this id and your observations.";
          std::string b64 = base64_encode(png);
          send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
            {"content", json::array({
              {{"type","text"},{"text",id_msg}},
              {{"type","image"},{"data",b64},{"mimeType","image/png"}}
            })}
          }}});
        }
        std::remove(tmp.c_str());

      } else if (tool == "amend_screenshot") {
        std::string scr_id      = args.value("id","");
        std::string observations = args.value("observations","");
        if (g_screenshot_dir.empty()) {
          send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
            {"code",-32000},{"message","amend_screenshot: no screenshot-dir configured (start rooture with --screenshot-dir)."}
          }}});
        } else if (scr_id.empty() || scr_id.find("(none") == 0) {
          send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
            {"code",-32000},{"message","amend_screenshot: invalid id '"+scr_id+"'."}
          }}});
        } else {
          // Write sidecar markdown file
          std::string md_name = scr_id + ".md";
          std::string md_path = g_screenshot_dir + "/" + md_name;
          std::ofstream md(md_path);
          md << "# Screenshot: " << scr_id << ".png\n\n"
             << "## Observations\n\n"
             << observations << "\n";
          md.close();

          // Commit it
          auto shell_escape = [](const std::string& s) {
            std::string r = "'";
            for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
            return r + "'";
          };
          // Use first line of observations as commit subject
          std::string first_line = observations.substr(0, observations.find('\n'));
          if (first_line.size() > 72) first_line = first_line.substr(0, 72) + "…";
          std::string commit_msg = "Annotate " + scr_id + ": " + first_line;
          std::string cmd =
            "git -C " + shell_escape(g_screenshot_dir) +
            " add " + shell_escape(md_name) +
            " && git -C " + shell_escape(g_screenshot_dir) +
            " commit -m " + shell_escape(commit_msg) +
            " >/dev/null 2>&1";
          std::system(cmd.c_str());

          send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
            {"content", json::array({{{"type","text"},{"text","Observations recorded for "+scr_id}}})}
          }}});
        }

      } else if (tool == "reload") {
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text","Reloading rooture with new binary..."}}})}
        }}});
        fflush(g_mcp_out);
        // Restore fd 1 to the real MCP stdout so the new process can set up
        // correctly (it will dup+redirect fd 1 again during its own init).
        dup2(fileno(g_mcp_out), STDOUT_FILENO);
        // Close everything except stdin/stdout/stderr so the new image starts
        // with a clean fd table.
        { int maxfd = (int)sysconf(_SC_OPEN_MAX);
          if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
          for (int fd = 3; fd < maxfd; fd++) close(fd); }
        // Signal the new process that it is a hot-reload so it sends
        // notifications/tools/list_changed without waiting for initialize.
        setenv("ROOTURE_MCP_RELOAD", "1", 1);
        execv(g_argv[0], g_argv);
        perror("execv");
        std::exit(1);

      } else {
        send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
          {"code",-32601},{"message","Unknown tool: "+tool}
        }}});
      }
    }
    // Other methods (e.g. ping, notifications) are silently ignored.
  }

  // EOF on stdin — signal main thread to exit.
  uint32_t eof = 0;
  write(g_pipe_fds[1], &eof, sizeof(eof));
}

int main(int argc, char** argv) {
  g_argv = argv;  // save before TApplication may shuffle argv
  const char* g_script_file = nullptr;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--debug") == 0)
      g_debug = true;
    else if (strcmp(argv[i], "--mcp") == 0)
      g_mcp_mode = true;
    else if (strcmp(argv[i], "--screenshot-dir") == 0 && i + 1 < argc)
      g_screenshot_dir = argv[++i];
    else if (argv[i][0] != '-' && !g_script_file)
      g_script_file = argv[i];
  }
  // Detect execv-based hot-reload: the previous process set this env var.
  if (getenv("ROOTURE_MCP_RELOAD")) {
    g_mcp_reload = true;
    unsetenv("ROOTURE_MCP_RELOAD");  // clear so a further reload doesn't re-trigger
  }

  /* In MCP mode save the real stdout fd for JSON-RPC, then redirect fd 1 to
     stderr so that ROOT internals (TEveManager, TApplication, etc.) cannot
     corrupt the JSON-RPC stream with their own print statements.  All ROOT
     output will appear on the terminal's stderr where it is still useful. */
  if (g_mcp_mode) {
    int saved = dup(STDOUT_FILENO);
    g_mcp_out = fdopen(saved, "w");
    dup2(STDERR_FILENO, STDOUT_FILENO);
  }

  /* Create Some Parsers */
  Float32   = mpc_new("float32");
  Floating  = mpc_new("floating");
  Number    = mpc_new("number");
  Symbol    = mpc_new("symbol");
  String    = mpc_new("string");
  Comment   = mpc_new("comment");
  Inlinestr = mpc_new("inlinestr");
  QexprItem = mpc_new("qexpr_item");
  Qexpr     = mpc_new("qexpr");
  Sexpr     = mpc_new("sexpr");
  Expr      = mpc_new("expr");
  Lispy     = mpc_new("lispy");

  /* Define them with the following Language */
  mpca_lang(MPCA_LANG_DEFAULT,
    "                                                              \
      float32  : /-?[0-9]+[.][0-9]*[eE][+-]?[0-9]+f/             \
               | /-?[0-9]+[.][0-9]*f/                             \
               | /-?[.][0-9]+[eE][+-]?[0-9]+f/                    \
               | /-?[.][0-9]+f/                                    \
               | /-?[0-9]+[eE][+-]?[0-9]+f/                       \
               | /-?[0-9]+f/ ;                                     \
      floating : /-?[0-9]+[.][0-9]*[eE][+-]?[0-9]+/               \
               | /-?[0-9]+[.][0-9]*/                               \
               | /-?[.][0-9]+[eE][+-]?[0-9]+/                      \
               | /-?[.][0-9]+/                                      \
               | /-?[0-9]+[eE][+-]?[0-9]+/ ;                      \
      number   : /-?[0-9]+/ ;                                     \
      symbol   : /[a-zA-Z0-9_+\\-*\\/\\\\=<>!&.:@~?%]+/ ;              \
      string   : /\"(\\\\.|[^\"])*\"/ ;                           \
      comment    : /;[^\\r\\n]*/ ;                                \
      inlinestr  : /[^}\\n]*/ ;                                   \
      qexpr_item : '|' <inlinestr> | <expr> ;                     \
      sexpr      : '(' <expr>* ')' ;                              \
      qexpr      : '{' <qexpr_item>* '}' ;                        \
      expr       : <float32> | <floating> | <number> | <symbol>   \
                 | <string> | <comment> | <sexpr> | <qexpr>;      \
      lispy    : /^/ <expr>* /$/ ;                                \
    ",
  Float32, Floating, Number, Symbol, String, Comment, Inlinestr, QexprItem, Sexpr, Qexpr, Expr, Lispy);

  /* Build the file search path */
  std::string exe_dir = executable_dir();
  load_path.push_back(exe_dir + "/../share/rooture");

  /* The environment */
  lenv* e = lenv_new();
  lenv_add_builtins(e);

  /* Set up the communication pipe between input thread and main thread */
  if (pipe(g_pipe_fds) != 0) { perror("pipe"); return 1; }

  /* In MCP mode create the reply pipe (main→MCP thread) */
  if (g_mcp_mode) {
    if (pipe(g_mcp_reply_fds) != 0) { perror("pipe(mcp_reply)"); return 1; }
  }

  /* Bootstrap TApplication so ROOT graphics/gSystem work */
  TApplication app("rooture", &argc, argv);

  /* Thread pool for (future ...) — sized to logical CPU count */
  rut_pool_create(std::max(1u, std::thread::hardware_concurrency()));

  /* Callback pipe: rooture lambdas connected to ROOT signals are deferred here
     so they run in the event loop, not inside Cling's slot-dispatch context. */
  if (pipe(g_cb_pipe) == 0) {
    fcntl(g_cb_pipe[0], F_SETFL, O_NONBLOCK);
    gSystem->AddFileHandler(rut_make_callback_handler(g_cb_pipe[0], e));
  }

  /* Cling dispatch pipe: future threads write here to wake the event loop when
     they have Cling work that must run on the main thread. */
  if (pipe(g_cling_pipe) == 0) {
    fcntl(g_cling_pipe[0], F_SETFL, O_NONBLOCK);
    gSystem->AddFileHandler(rut_make_cling_handler(g_cling_pipe[0]));
  }

  /* Script mode: load a file and exit without starting the event loop */
  if (g_script_file) {
    mpc_result_t r;
    if (mpc_parse_contents(g_script_file, Lispy, &r)) {
      lval* expr = lval_read((mpc_ast_t*)r.output);
      mpc_ast_delete((mpc_ast_t*)r.output);
      while (expr->count) {
        lval* x = lval_eval(e, lval_pop(expr, 0));
        if (x->type == LVAL_ERR) { lval_println(x); lval_del(x); lval_del(expr); exit(1); }
        lval_del(x);
      }
      lval_del(expr);
    } else {
      char* err_msg = mpc_err_string(r.error);
      mpc_err_delete(r.error);
      fprintf(stderr, "%s\n", err_msg);
      free(err_msg);
      exit(1);
    }
    exit(0);
  }

  /* Register the pipe read-end with gSystem's event loop */
  PipeHandler* ph = new PipeHandler(e);
  ph->Add();

  /* Start the appropriate I/O thread */
  std::thread io_thread(g_mcp_mode ? mcp_thread_fn : input_thread_fn);

  /* Run ROOT's event loop on the main thread */
  gSystem->Run();

  io_thread.join();

  ph->Remove();
  delete ph;

  rut_pool_destroy();

  lenv_del(e);

  /* Undefine and delete our parsers */
  mpc_cleanup(12,
    Float32, Number, Floating, Symbol, String, Comment, Inlinestr, QexprItem,
    Sexpr,  Qexpr,  Expr,   Lispy);

  return 0;
}
