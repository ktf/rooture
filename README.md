# ROOTure

**ROOTure** |ˈrʌtʃə| is a Lisp dialect where [ROOT](https://root.cern.ch) is a
first-class citizen. Instead of writing C++ macros or Python scripts, you get an
interactive REPL that speaks ROOT natively — create histograms, run RDataFrame
pipelines, fit functions, and render canvases, all in a concise Lisp syntax.

If you know [Clojure](http://clojure.org) and its relationship with Java, you get
the idea immediately.

---

## Quick taste

```scheme
(load stdlib.rut)

(doto (new TH1F "h" "Gaussian;x;Events" 100 -4. 4.)
  {SetFillColorAlpha 9 0.35}
  {SetLineColor 9}
  {FillRandom gaus 50000}
  {Draw})
```

![Gaussian histogram](docs/doto_histogram.png)

---

## AI-assisted analysis

ROOTure is designed for **interactive, AI-assisted data analysis**.
Two properties make it a particularly good fit:

**Lisp syntax is trivial for LLMs to generate correctly.**
There is no operator precedence, no implicit semicolons, no indentation rules.
Every expression is a fully self-contained tree wrapped in parentheses.
An LLM can write, extend, or patch rooture code with far fewer syntactic errors
than equivalent C++ or Python — and a bad expression fails cleanly at the REPL
rather than segfaulting or leaving the interpreter in a broken state.

**The MCP server closes the feedback loop.**
ROOTure embeds a [Model Context Protocol](https://modelcontextprotocol.io) server
so a connected AI assistant can evaluate expressions, read back results, capture
canvas images, and iterate — all without you copying and pasting between windows.

### Example: vibe-coding a fit GUI

The interactive fit GUI in [`examples/fit_gui.rut`](examples/fit_gui.rut) was built
entirely in conversation with Claude via MCP:

<table>
<tr>
<th width="55%">Prompt</th>
<th>Result</th>
</tr>
<tr>
<td>

> "Create a histogram of a Gaussian and draw it."

```scheme
(doto (new TH1F "h" "Gaussian;x;Events" 100 -4. 4.)
  {FillRandom gaus 50000}
  {Draw})
```

Claude calls `get_canvas` to see the raw distribution.

</td>
<td>

![Initial histogram](docs/doto_histogram.png)

</td>
</tr>
<tr>
<td>

> "Add sliders for mean, sigma, and number of entries,
> and a live chi² display. Redraw on every slider move."

Claude writes [`examples/fit_gui.rut`](examples/fit_gui.rut),
loads it, and calls `get_window` to inspect the layout.

</td>
<td>

![Fit GUI](docs/fit_gui.png)

</td>
</tr>
</table>

The full session diary — including the spline feature, two bugs found and fixed,
and a χ²/ndf display added for the smooth mode — is documented in
[`docs/spline-vibe-coding.md`](docs/spline-vibe-coding.md).

The `annotate` form lets you leave semantic notes in the script that the assistant
can read back via `list_annotations` — marking the meaningful knobs so it knows
exactly what to suggest changes to:

```scheme
(def {nBins} 128)
(annotate nBins "Histogram binning — increase for better mass resolution at the cost of stats")

(def {fitRange} {2.9 3.2})
(annotate fitRange "J/psi fit window in GeV — widen to include more background")
```

The assistant calls `list_annotations`, sees these descriptions, and can propose
`(def {nBins} 256)` with full context — rather than guessing what the numbers mean.

---

## RDataFrame pipelines

The `->` threading macro chains RDataFrame operations cleanly:

```scheme
(-> (new ROOT::RDataFrame 100000)
    {.Define "x" "gRandom->Gaus(3.1, 0.05)"}
    {.Histo1D "x"}
    {.DrawClone})
```

![RDataFrame demo](docs/rdf_demo.png)

For more complex pipelines, named intermediate results bind naturally.
The example below reproduces the CMS dimuon spectrum from open data — full
spectrum on the left (log scale), J/ψ window on the right:

```scheme
(load stdlib.rut)
(def {filteredEvents}
  (-> (::FromCSV ROOT::RDF "http://root.cern/files/tutorials/df014_CsvDataSource_MuRun2010B.csv")
    {.Filter | Q1 * Q2 == -1}
    {.Define "m" | sqrt(pow(E1 + E2, 2) - (pow(px1 + px2, 2) + pow(py1 + py2, 2) + pow(pz1 + pz2, 2)))}))

(def {fullSpectrum} (.Histo1D filteredEvents
  (new ROOT::RDF::TH1DModel "Spectrum" "Subset of CMS Run 2010B;#mu#mu mass [GeV];Events" 1024 2. 110.) "m"))
(def {jpsi} (.Histo1D
  (-> filteredEvents {.Filter | m < 3.25 && m > 2.95})
  (new ROOT::RDF::TH1DModel "jpsi" "Subset of CMS Run 2010B: J/#psi window;#mu#mu mass [GeV];Events" 128 2.95 3.25) "m"))

(def {dualCanvas} (new TCanvas "DualCanvas" "DualCanvas" 800 512))
(.Divide dualCanvas 2 1)
(doto (.cd dualCanvas 1) {SetLogx} {SetLogy})
(.DrawClone fullSpectrum "Hist")
(.cd dualCanvas 2)
(doto jpsi {SetMarkerStyle 20} {DrawClone | HistP})
```

![CMS dimuon spectrum](docs/df014_CsvDataSource.png)

---

## Key features

- **Interactive REPL** with syntax highlighting, history, and tab completion
- **Seamless ROOT interop** — any ROOT class, method, or global is directly accessible
- **`doto`** — apply multiple methods to one object (à la Clojure)
- **`->`** — thread a value through a chain of method calls
- **`@name`** — retrieve any named ROOT object via `gROOT->FindObject`
- **Lambdas** — pass rooture functions as C++ callables (e.g. `RDataFrame::Define`)
- **`|` tail strings** — write unquoted strings at the end of a Q-expression: `{.Filter | pt > 10}`
- **`annotate`** — attach semantic notes to symbols; exposed to AI assistants via MCP
- **MCP server** — connect Claude or any MCP-capable AI assistant for AI-assisted analysis

---

## MCP server

Start rooture with the MCP flag and configure your AI assistant to connect to it:

```sh
./build/rooture --mcp
```

With Claude Code, add a `.mcp.json` to your project directory:

```json
{
  "mcpServers": {
    "rooture": {
      "command": "/path/to/rooture",
      "args": ["--mcp"]
    }
  }
}
```

The assistant can then evaluate expressions, read back results, and capture canvases
without you copying anything between windows.

Available MCP tools:

| Tool | Description |
|------|-------------|
| `eval` | Evaluate any rooture expression |
| `list_symbols` | List all user-defined symbols |
| `list_annotations` | List all `annotate`d symbols with their descriptions |
| `list_canvases` | List open ROOT canvases |
| `get_canvas` | Capture a canvas as a PNG image |
| `get_window` | Capture a GUI window (`TGFrame`) as a PNG image |
| `reload` | Restart the server after rebuilding |

---

## Building

Requires CMake and a ROOT installation (≥ 6.20):

```sh
cmake -DROOT_ROOT=<path-to-root> -B build
cmake --build build
```

Run interactively:

```sh
./build/rooture
```

Or execute a script:

```sh
./build/rooture examples/df014_CsvDataSource.rut
```

---

## Language overview

See **[docs/language.md](docs/language.md)** for the full language reference.

A quick taste:

```scheme
(def {h} (new TH1F "foo" "bar" 100 -5. 5.))   ; create object
(.FillRandom h gaus 10000)                      ; call method
(.Draw h)

(-> dataFrame                                   ; method chaining
  {.Filter | pt > 10}
  {.Histo1D "pt"}
  {.DrawClone})
```

---

## Examples

| File | Description |
|------|-------------|
| `examples/draw_histo.rut` | Basic histogram with fill style |
| `examples/df000_simple.rut` | Minimal RDataFrame pipeline |
| `examples/df014_CsvDataSource.rut` | CMS dimuon spectrum from CSV |
| `examples/df101_h1Analysis.rut` | H1 D* analysis with fitting |
| `examples/assembly.rut` | TGeo geometry assembly |
| `examples/fit_gui.rut` | Interactive fit GUI with sliders and live chi² output |

---

## Background

The interpreter started from the ["Build Your Own Lisp"](http://www.buildyourownlisp.com)
tutorial and evolved into a ROOT-native REPL. It uses ROOT's Cling/LLVM JIT to dispatch
method calls, so you get the full power of C++ ROOT without writing C++.
