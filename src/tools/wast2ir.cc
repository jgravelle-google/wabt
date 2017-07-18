/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>

#include "config.h"

#include "binary-writer.h"
#include "binary-writer-spec.h"
#include "common.h"
#include "error-handler.h"
#include "ir.h"
#include "option-parser.h"
#include "resolve-names.h"
#include "stream.h"
#include "validator.h"
#include "wast-parser.h"
#include "writer.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"

using namespace wabt;

static const char* s_infile;
static const char* s_outfile;
static int s_verbose;
static WriteBinaryOptions s_write_binary_options;
static bool s_validate = true;
static WastParseOptions s_parse_options;

static std::unique_ptr<FileStream> s_log_stream;

static const char s_description[] =
R"(  read a file in the wasm s-expression format, check it for errors, and
  convert it to LLVM IR.
)";

static void parse_options(int argc, char* argv[]) {
  OptionParser parser("wast2ir", s_description);

  parser.AddOption('v', "verbose", "Use multiple times for more info", []() {
    s_verbose++;
    s_log_stream = FileStream::CreateStdout();
    s_write_binary_options.log_stream = s_log_stream.get();
  });
  parser.AddHelpOption();
  parser.AddOption("debug-parser", "Turn on debugging the parser of wast files",
                   []() { s_parse_options.debug_parsing = true; });
  parser.AddOption('o', "output", "FILE", "output wasm binary file",
                   [](const char* argument) { s_outfile = argument; });
  parser.AddOption("debug-names",
                   "Write debug names to the generated binary file",
                   []() { s_write_binary_options.write_debug_names = true; });
  parser.AddOption("no-check", "Don't check for invalid modules",
                   []() { s_validate = false; });
  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) { s_infile = argument; });

  parser.Parse(argc, argv);
}

Result write_ir_file(const Module* module, const char* filename) {
  llvm::LLVMContext context;
  llvm::Module* ir = new llvm::Module("test", context);
  printf("Woop: %s , %p\n", filename, ir);
  ir->dump();
  // std::ofstream file;
  // file.open(filename, std::ios::out | std::ios::binary);
  // ir->print(file, nullptr);
  return Result::Ok;
}

int ProgramMain(int argc, char** argv) {
  init_stdio();

  parse_options(argc, argv);

  std::unique_ptr<WastLexer> lexer = WastLexer::CreateFileLexer(s_infile);
  if (!lexer)
    WABT_FATAL("unable to read file: %s\n", s_infile);

  ErrorHandlerFile error_handler(Location::Type::Text);
  Script* script;
  Result result =
      parse_wast(lexer.get(), &script, &error_handler, &s_parse_options);

  if (Succeeded(result))
    result = resolve_names_script(lexer.get(), script, &error_handler);

  if (Succeeded(result) && s_validate)
    result = validate_script(lexer.get(), script, &error_handler);

  if (Succeeded(result)) {
    const Module* module = script->GetFirstModule();
    if (module) {
      result = write_ir_file(module, s_outfile);
    } else {
      WABT_FATAL("no module found\n");
    }
  }

  delete script;
  return result != Result::Ok;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
