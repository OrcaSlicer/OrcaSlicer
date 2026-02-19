
# MD4C Readme

* Home: http://github.com/mity/md4c
* Wiki: http://github.com/mity/md4c/wiki
* Issue tracker: http://github.com/mity/md4c/issues

MD4C stands for "Markdown for C" and that's exactly what this project is about.


## What is Markdown

In short, Markdown is the markup language this `README.md` file is written in.

The following resources can explain more if you are unfamiliar with it:
* [Wikipedia article](http://en.wikipedia.org/wiki/Markdown)
* [CommonMark site](http://commonmark.org)


## What is MD4C

MD4C is Markdown parser implementation in C, with the following features:

* **Compliance:** Generally, MD4C aims to be compliant to the latest version of
  [CommonMark specification](http://spec.commonmark.org/). Currently, we are
  fully compliant to CommonMark 0.31.

* **Extensions:** MD4C supports some commonly requested and accepted extensions.
  See below.

* **Performance:** MD4C is [very fast](https://talk.commonmark.org/t/2520).

* **Compactness:** MD4C parser is implemented in one source file and one header
  file. There are no dependencies other than standard C library.

* **Embedding:** MD4C parser is easy to reuse in other projects, its API is
  very straightforward: There is actually just one function, `md_parse()`.

* **Push model:** MD4C parses the complete document and calls few callback
  functions provided by the application to inform it about a start/end of
  every block, a start/end of every span, and with any textual contents.

* **Portability:** MD4C builds and works on Windows and POSIX-compliant OSes.
  (It should be simple to make it run also on most other platforms, at least as
  long as the platform provides C standard library, including a heap memory
  management.)

* **Encoding:** MD4C by default expects UTF-8 encoding of the input document.
  But it can be compiled to recognize ASCII-only control characters (i.e. to
  disable all Unicode-specific code), or (on Windows) to expect UTF-16 (i.e.
  what is on Windows commonly called just "Unicode"). See more details below.

* **Permissive license:** MD4C is available under the [MIT license](LICENSE.md).


## Using MD4C

### Parsing Markdown

If you need just to parse a Markdown document, you need to include `md4c.h`
and link against MD4C library (`-lmd4c`); or alternatively add `md4c.[hc]`
directly to your code base as the parser is only implemented in the single C
source file.

The main provided function is `md_parse()`. It takes a text in the Markdown
syntax and a pointer to a structure which provides pointers to several callback
functions.

As `md_parse()` processes the input, it calls the callbacks (when entering or
leaving any Markdown block or span; and when outputting any textual content of
the document), allowing application to convert it into another format or render
it onto the screen.


### Converting to HTML

If you need to convert Markdown to HTML, include `md4c-html.h` and link against
MD4C-HTML library (`-lmd4c-html`); or alternatively add the sources `md4c.[hc]`,
`md4c-html.[hc]` and `entity.[hc]` into your code base.

To convert a Markdown input, call `md_html()` function. It takes the Markdown
input and calls the provided callback function. The callback is fed with
chunks of the HTML output. Typical callback implementation just appends the
chunks into a buffer or writes them to a file.


## Markdown Extensions

The default behavior is to recognize only Markdown syntax defined by the
[CommonMark specification](http://spec.commonmark.org/).

However, with appropriate flags, the behavior can be tuned to enable some
extensions:

* With the flag `MD_FLAG_COLLAPSEWHITESPACE`, a non-trivial whitespace is
  collapsed into a single space.

* With the flag `MD_FLAG_TABLES`, GitHub-style tables are supported.

* With the flag `MD_FLAG_TASKLISTS`, GitHub-style task lists are supported.

* With the flag `MD_FLAG_STRIKETHROUGH`, strike-through spans are enabled
  (text enclosed in tilde marks, e.g. `~foo bar~`).

* With the flag `MD_FLAG_PERMISSIVEURLAUTOLINKS` permissive URL autolinks
  (not enclosed in `<` and `>`) are supported.

* With the flag `MD_FLAG_PERMISSIVEEMAILAUTOLINKS`, permissive e-mail
  autolinks (not enclosed in `<` and `>`) are supported.

* With the flag `MD_FLAG_PERMISSIVEWWWAUTOLINKS` permissive WWW autolinks
  without any scheme specified (e.g. `www.example.com`) are supported. MD4C
  then assumes `http:` scheme.

* With the flag `MD_FLAG_LATEXMATHSPANS` LaTeX math spans (`$...$`) and
  LaTeX display math spans (`$$...$$`) are supported. (Note though that the
  HTML renderer outputs them verbatim in a custom tag `<x-equation>`.)

* With the flag `MD_FLAG_WIKILINKS`, wiki-style links (`[[link label]]` and
  `[[target article|link label]]`) are supported. (Note that the HTML renderer
  outputs them in a custom tag `<x-wikilink>`.)

* With the flag `MD_FLAG_UNDERLINE`, underscore (`_`) denotes an underline
  instead of an ordinary emphasis or strong emphasis.

Few features of CommonMark (those some people see as mis-features) may be
disabled with the following flags:

* With the flag `MD_FLAG_NOHTMLSPANS` or `MD_FLAG_NOHTMLBLOCKS`, raw inline
  HTML or raw HTML blocks respectively are disabled.

* With the flag `MD_FLAG_NOINDENTEDCODEBLOCKS`, indented code blocks are
  disabled.


## Input/Output Encoding

The CommonMark specification declares that any sequence of Unicode code points
is a valid CommonMark document.

But, under a closer inspection, Unicode plays any role in few very specific
situations when parsing Markdown documents:

1. For detection of word boundaries when processing emphasis and strong
   emphasis, some classification of Unicode characters (whether it is
   a whitespace or a punctuation) is needed.

2. For (case-insensitive) matching of a link reference label with the
   corresponding link reference definition, Unicode case folding is used.

3. For translating HTML entities (e.g. `&amp;`) and numeric character
   references (e.g. `&#35;` or `&#xcab;`) into their Unicode equivalents.

   However note MD4C leaves this translation on the renderer/application; as
   the renderer is supposed to really know output encoding and whether it
   really needs to perform this kind of translation. (For example, when the
   renderer outputs HTML, it may leave the entities untranslated and defer the
   work to a web browser.)

MD4C relies on this property of the CommonMark and the implementation is, to
a large degree, encoding-agnostic. Most of MD4C code only assumes that the
encoding of your choice is compatible with ASCII. I.e. that the codepoints
below 128 have the same numeric values as ASCII.

Any input MD4C does not understand is simply seen as part of the document text
and sent to the renderer's callback functions unchanged.

The two situations (word boundary detection and link reference matching) where
MD4C has to understand Unicode are handled as specified by the following
preprocessor macros (as specified at the time MD4C is being built):

* If preprocessor macro `MD4C_USE_UTF8` is defined, MD4C assumes UTF-8 for the
  word boundary detection and for the case-insensitive matching of link labels.

  When none of these macros is explicitly used, this is the default behavior.

* On Windows, if preprocessor macro `MD4C_USE_UTF16` is defined, MD4C uses
  `WCHAR` instead of `char` and assumes UTF-16 encoding in those situations.
  (UTF-16 is what Windows developers usually call just "Unicode" and what
  Win32API generally works with.)

  Note that because this macro affects also the types in `md4c.h`, you have
  to define the macro both when building MD4C as well as when including
  `md4c.h`.

  Also note this is only supported in the parser (`md4c.[hc]`). The HTML
  renderer does not support this and you will have to write your own custom
  renderer to use this feature.

* If preprocessor macro `MD4C_USE_ASCII` is defined, MD4C assumes nothing but
  an ASCII input.

  That effectively means that non-ASCII whitespace or punctuation characters
  won't be recognized as such and that link reference matching will work in
  a case-insensitive way only for ASCII letters (`[a-zA-Z]`).


## Documentation

The API of the parser is quite well documented in the comments in the `md4c.h`.
Similarly, the markdown-to-html API is described in its header `md4c-html.h`.

There is also [project wiki](http://github.com/mity/md4c/wiki) which provides
some more comprehensive documentation. However note it is incomplete and some
details may be somewhat outdated.


## FAQ

**Q: How does MD4C compare to other Markdown parsers?**

**A:** Some other implementations combine Markdown parser and HTML generator
into a single entangled code hidden behind an interface which just allows the
conversion from Markdown to HTML. They are often unusable if you want to
process the input in any other way.

Second, most parsers (if not all of them; at least within the scope of C/C++
language) are full DOM-like parsers: They construct abstract syntax tree (AST)
representation of the whole Markdown document. That takes time and it leads to
bigger memory footprint.

Building AST is completely fine as long as you need it. If you don't, there is
a very high chance that using MD4C will be substantially faster and less hungry
in terms of memory consumption.

Last but not least, some Markdown parsers are implemented in a naive way. When
fed with a [smartly crafted input pattern](test/pathological_tests.py), they
may exhibit quadratic (or even worse) parsing times. What MD4C can still parse
in a fraction of second may turn into long minutes or possibly hours with them.
Hence, when such a naive parser is used to process an input from an untrusted
source, the possibility of denial-of-service attacks becomes a real danger.

A lot of our effort went into providing linear parsing times no matter what
kind of crazy input MD4C parser is fed with. (If you encounter an input pattern
which leads to a sub-linear parsing times, please do not hesitate and report it
as a bug.)

**Q: Does MD4C perform any input validation?**

**A:** No. And we are proud of it. :-)

CommonMark specification states that any sequence of Unicode characters is
a valid Markdown document. (In practice, this more or less always means UTF-8
encoding.)

In other words, according to the specification, it does not matter whether some
Markdown syntax construction is in some way broken or not. If it's broken, it
won't be recognized and the parser should see it just as a verbatim text.

MD4C takes this a step further: It sees any sequence of bytes as a valid input,
following completely the GIGO philosophy (garbage in, garbage out). I.e. any
ill-formed UTF-8 byte sequence will propagate to the respective callback as
a part of the text.

If you need to validate that the input is, say, a well-formed UTF-8 document,
you have to do it on your own. The easiest way how to do this is to simply
validate the whole document before passing it to the MD4C parser.


## License

MD4C is covered with MIT license, see the file `LICENSE.md`.


## Links to Related Projects

Ports and bindings to other languages:

* [commonmark-d](https://github.com/AuburnSounds/commonmark-d):
  Port of MD4C to D language.

* [markdown-wasm](https://github.com/rsms/markdown-wasm):
  Port of MD4C to WebAssembly.

* [PyMD4C](https://github.com/dominickpastore/pymd4c):
  Python bindings for MD4C

Software using MD4C:

* [imgui_md](https://github.com/mekhontsev/imgui_md):
  Markdown renderer for [Dear ImGui](https://github.com/ocornut/imgui)

* [MarkDown Monolith Assembler](https://github.com/1Hyena/mdma):
  A command line tool for building browser-based books.

* [QOwnNotes](https://www.qownnotes.org/):
  A plain-text file notepad and todo-list manager with markdown support and
  ownCloud / Nextcloud integration.

* [Qt](https://www.qt.io/):
  Cross-platform C++ GUI framework.

* [Textosaurus](https://github.com/martinrotter/textosaurus):
  Cross-platform text editor based on Qt and Scintilla.

* [8th](https://8th-dev.com/):
  Cross-platform concatenative programming language.
