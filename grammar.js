/// @file grammar.js — tree-sitter grammar for the rooture Lisp dialect.
///
/// Regenerate the parser after editing:
///   tree-sitter generate
/// This produces src/parser.c which is compiled by the tree-sitter-rooture
/// alibuild recipe.

module.exports = grammar({
  name: "rooture",

  // Whitespace and comments are ignored between every node.
  extras: ($) => [/\s+/, $.comment],

  rules: {
    source_file: ($) => repeat($._expr),

    _expr: ($) =>
      choice(
        $.sexpr,
        $.qexpr,
        $.float,        // must come before number (more specific)
        $.number,
        $.string,
        $.dot_method,   // must come before symbol (.Method)
        $.named_ref,    // must come before symbol (@name)
        $.symbol,
      ),

    // S-expression  (evaluated)
    sexpr: ($) => seq("(", repeat($._expr), ")"),

    // Q-expression  (quoted / unevaluated)
    // {a b | raw tail text} — everything after | up to } becomes a string
    qexpr: ($) => choice(
      seq("{", repeat($._expr), "|", $.tailstr, "}"),
      seq("{", repeat($._expr), "}"),
    ),

    // Raw tail text after | inside a Q-expression (cannot contain })
    tailstr: ($) => token(/[^}]*/),

    // Literals
    float:  ($) => token(choice(/-?[0-9]+\.[0-9]*/, /-?\.[0-9]+/)),
    number: ($) => token(/-?[0-9]+/),
    string: ($) => token(/"([^"\\]|\\.)*"/),

    // .Method — shorthand for (. Method ...)
    dot_method: ($) => token(/\.[a-zA-Z_][a-zA-Z0-9_]*/),

    // @name — reference to a ROOT TNamed object by name
    named_ref: ($) => token(/@[a-zA-Z0-9_]+/),

    // Everything else is a symbol (operators, ROOT class paths, …)
    symbol: ($) => token(/[a-zA-Z0-9_+\-*\/\\=<>!&.:$]+/),

    // Line comment — not listed in extras so it appears in the AST
    comment: ($) => token(/;[^\r\n]*/),
  },
});
