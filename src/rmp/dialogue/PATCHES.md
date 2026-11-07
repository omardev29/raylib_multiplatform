MODIFIED: Yarn Spinner's compiler and runtime, ported to C++.

src/rmp/dialogue/ is not a copy of YarnSpinnerTool/YarnSpinner (C#, MIT, its
notice in `LICENSE-YarnSpinner.md` beside this file): it is a port of its design to C++
for rmp::Dialogue, measured against the original's own test cases
(tests/dialogue/yarn/, at the commit in tests/dialogue/yarn/COMMIT).

| File | From | What differs |
|---|---|---|
| yarn_parse.cpp | YarnSpinnerLexer.g4, YarnSpinnerParser.g4, IndentAwareLexer.cs | hand-written, a line at a time; no ANTLR |
| yarn_compile.cpp | Compiler.cs, NodeGroupCompiler.cs, CodeGenerationVisitor.cs, TypeSolver/ | a simpler type inference; a function's result may stay untyped where any value goes |
| yarn_vm.cpp, yarn_value.cpp | VirtualMachine.cs, Dialogue.cs, Library.cs, Saliency/ | a budget of instructions and of detour depth, so a loop with no line stops |
| yarn_markup.cpp | YarnSpinner.Markup/LineParser.cs | select, plural and ordinal by the framework's CLDR table (src/rmp/text.cpp) |
| yarn_bytes.cpp | yarn_spinner.proto | a format of our own instead of protobuf |
