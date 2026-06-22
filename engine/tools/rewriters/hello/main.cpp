// orca-hello — libtooling smoke test.
//
// Loads compile_commands.json (via CommonOptionsParser) and traverses the
// AST of the named source file(s), counting function definitions in the
// main file and printing the first few. If this runs successfully against
// a file from src/slic3r/GUI/, the libtooling toolchain is wired up for
// real rewriter work in Phase 0.4a.

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

#include <cstdio>
#include <memory>

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory HelloCategory("orca-hello options");

class HelloVisitor : public RecursiveASTVisitor<HelloVisitor> {
public:
    explicit HelloVisitor(ASTContext& ctx) : ctx_(ctx) {}

    bool VisitFunctionDecl(FunctionDecl* fn) {
        if (!fn->hasBody()) return true;
        const auto& sm = ctx_.getSourceManager();
        if (!sm.isInMainFile(fn->getLocation())) return true;
        ++count_;
        if (count_ <= 5) {
            std::printf("  fn: %-40s @ %s\n",
                        fn->getNameAsString().c_str(),
                        fn->getLocation().printToString(sm).c_str());
        }
        return true;
    }

    int count() const { return count_; }

private:
    ASTContext& ctx_;
    int         count_ = 0;
};

class HelloConsumer : public ASTConsumer {
public:
    void HandleTranslationUnit(ASTContext& ctx) override {
        HelloVisitor v(ctx);
        v.TraverseDecl(ctx.getTranslationUnitDecl());
        std::printf("orca-hello: %d function definitions in main file\n", v.count());
    }
};

class HelloAction : public ASTFrontendAction {
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<HelloConsumer>();
    }
};

// The OrcaSlicer build uses GCC; libclang-based tools choke on GCC-only flags
// in compile_commands.json. Strip them, drop the precompiled-header include
// (GCC .gch files aren't clang-compatible), and inject clang's resource dir
// so the bundled stddef.h/float.h wrappers get found.
ArgumentsAdjuster stripGccOnlyFlags() {
    return [](const CommandLineArguments& args, llvm::StringRef) {
        CommandLineArguments out;
        out.reserve(args.size() + 1);
        out.push_back(args.empty() ? std::string("clang++") : args.front());
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "-fext-numeric-literals") continue;
            // GCC-only warning flags clang doesn't recognize.
            if (llvm::StringRef(a).contains("template-id-cdtor")) continue;
            if (a == "-include" && i + 1 < args.size() &&
                llvm::StringRef(args[i + 1]).contains("cmake_pch")) {
                ++i;
                continue;
            }
            out.push_back(a);
        }
        out.push_back("-resource-dir=" CLANG_RESOURCE_DIR);
        return out;
    };
}

} // namespace

int main(int argc, const char** argv) {
    auto parser = CommonOptionsParser::create(argc, argv, HelloCategory);
    if (!parser) {
        llvm::errs() << parser.takeError();
        return 1;
    }
    ClangTool tool(parser->getCompilations(), parser->getSourcePathList());
    tool.appendArgumentsAdjuster(stripGccOnlyFlags());
    return tool.run(newFrontendActionFactory<HelloAction>().get());
}
