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
#include <vector>

#include "config.h"

#include "common.h"
#include "error-handler.h"
#include "expr-visitor.h"
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
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

using namespace wabt;

#define CHECK_RESULT(expr)  \
  do {                      \
    if (Failed(expr))       \
      return Result::Error; \
  } while (0)

struct StackSlot {
  Expr* expr;
  llvm::Value* value;
};

class ExprStack {
private:
  std::vector<StackSlot> data;

public:
  void Push(StackSlot slot) {
    data.push_back(slot);
  }
  StackSlot Pop() {
    if (data.size() == 0) {
      WABT_FATAL("Popping an empty stack\n");
    }
    StackSlot ret = data[data.size() - 1];
    data.pop_back();
    return ret;
  }
};

class IRVisitor : public ExprVisitor::DelegateNop {
public:
  llvm::Module* ir;
  llvm::LLVMContext context;
  llvm::IRBuilder<> builder;
  llvm::Function* currentFunction;

  Module* module;
  Func* currentFunc;
  ExprVisitor visitor;
  ExprStack stack;

  IRVisitor() : builder(context), visitor(this) {
    ir = new llvm::Module("test", context);
  }
  ~IRVisitor() {
    delete ir;
  }

  Result VisitModule(Module* module_) {
    module = module_;

    CHECK_RESULT(CreateInitMemoryFunction());

    for (Func* func : module->funcs) {
      CHECK_RESULT(VisitFunc(func));
    }
    // for (Index i = 0; i < module->globals.size(); ++i)
    //   CHECK_RESULT(VisitGlobal(i, module->globals[i]));
    // for (Index i = 0; i < module->func_types.size(); ++i)
    //   CHECK_RESULT(VisitFuncType(i, module->func_types[i]));
    // for (Index i = 0; i < module->tables.size(); ++i)
    //   CHECK_RESULT(VisitTable(i, module->tables[i]));

    return Result::Ok;
  }

private:

  Result CreateInitMemoryFunction() {
    assert(module->memories.size() == 1);

    llvm::GlobalVariable* memory = LinearMemory();
    // memory->setSection(".membase");
    long memorySize = 10000000; // TODO: based on memory declaration
    memory->setInitializer(
      llvm::Constant::getNullValue(
        llvm::ArrayType::get(builder.getInt8Ty(), memorySize)));

    llvm::Function* function = GetMemoryInitFunction();
    llvm::BasicBlock* bb =
      llvm::BasicBlock::Create(context, "entry", function);
    builder.SetInsertPoint(bb);
    builder.CreateRetVoid();
    return Result::Ok;
  }

  llvm::Function* GetMemoryInitFunction() {
    llvm::Constant* c = ir->getOrInsertFunction(
      "___initializeWasmLinearMemory", llvm::Type::getVoidTy(context));
    return llvm::cast<llvm::Function>(c);
  }

  llvm::GlobalVariable* LinearMemory() {
    long memorySize = 10000000; // TODO: based on memory declaration
    llvm::ArrayType* array =
      llvm::ArrayType::get(builder.getInt8Ty(), memorySize);
    llvm::Constant* c = ir->getOrInsertGlobal("___wasmLinearMemory", array);
    return llvm::cast<llvm::GlobalVariable>(c);
  }

  Result VisitFunc(Func* func) {
    currentFunction = ToLLVMFunction(func);
    currentFunc = func;

    bool isImport = func->name == "$puts"; // TODO: detect this properly
    if (isImport) {
      return Result::Ok;
    }

    llvm::BasicBlock* bb =
      llvm::BasicBlock::Create(context, "entry", currentFunction);
    builder.SetInsertPoint(bb);

    if (func->name == "$main") {
      builder.CreateCall(GetMemoryInitFunction());
    }

    visitor.VisitFunc(func);
    return Result::Ok;
  }

  llvm::Function* ToLLVMFunction(Func* func) {
    llvm::FunctionType* funcType = ToLLVMFuncType(func);
    llvm::Constant* c =
      ir->getOrInsertFunction(func->name.c_str() + 1, funcType);
    return llvm::cast<llvm::Function>(c);
  }

  llvm::FunctionType* ToLLVMFuncType(Func* func) {
    llvm::Type* retType = ReturnType(func);
    std::vector<llvm::Type*> argTypes;
    for (int i = 0; i < func->GetNumParams(); ++i) {
      Type ty = func->GetParamType(i);
      argTypes.push_back(ToLLVMType(ty));
    }
    llvm::FunctionType* funcType =
      llvm::FunctionType::get(retType, argTypes, false);
    return funcType;
  }

  llvm::Type* ReturnType(Func* func) {
    assert(func->GetNumResults() <= 1);
    if (func->GetNumResults() == 0) {
      return llvm::Type::getVoidTy(context);
    } else {
      return ToLLVMType(func->GetResultType(0));
    }
  }

  llvm::Type* ToLLVMType(Type type) {
    switch (type) {
    case Type::I32:
      return llvm::Type::getInt32Ty(context);
    case Type::I64:
      return llvm::Type::getInt64Ty(context);
    case Type::F32:
      return llvm::Type::getFloatTy(context);
    case Type::F64:
      return llvm::Type::getDoubleTy(context);
    default:
      WABT_FATAL("Unsupported wasm->llvm type: %d\n", type);
    };
  }

public:
  Result OnCallExpr(CallExpr* call) override {
    Var var = call->var;
    Func* func = module->funcs[var.index()];
    llvm::Function* function = ToLLVMFunction(func);
    std::vector<llvm::Value*> args;
    for (int i = 0; i < func->GetNumParams(); ++i) {
      llvm::Value* value = stack.Pop().value;
      llvm::Value* gep = builder.CreateGEP(LinearMemory(), value);
      llvm::Value* cast = builder.CreateBitCast(gep, builder.getInt8PtrTy());
      llvm::Value* ptoi = builder.CreatePtrToInt(cast, value->getType());
      args.push_back(ptoi);
    }
    llvm::Value* value = builder.CreateCall(function, args);
    stack.Push({call, value});
    return Result::Ok;
  }

  Result OnConstExpr(ConstExpr* c) override {
    Const const_ = c->const_;
    llvm::Value* value;
    switch (const_.type) {
    case Type::I32:
      value = builder.getInt32(const_.u32);
      break;
    case Type::I64:
      value = builder.getInt64(const_.u64);
      break;
    default:
      WABT_FATAL("Unsupported const expr type: %d\n", const_.type);
    };
    stack.Push({c, value});
    return Result::Ok;
  }

  Result OnDropExpr(DropExpr* drop) override {
    stack.Pop();
    return Result::Ok;
  }

  Result OnReturnExpr(ReturnExpr* ret) override {
    if (currentFunc->GetNumResults() == 0) {
      builder.CreateRetVoid();
      return Result::Ok;
    }
    builder.CreateRet(stack.Pop().value);
    return Result::Ok;
  }
};

// ------ //

static const char* s_infile;
static const char* s_outfile;
static int s_verbose;
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
  });
  parser.AddHelpOption();
  parser.AddOption("debug-parser", "Turn on debugging the parser of wast files",
                   []() { s_parse_options.debug_parsing = true; });
  parser.AddOption('o', "output", "FILE", "output wasm binary file",
                   [](const char* argument) { s_outfile = argument; });
  parser.AddOption("no-check", "Don't check for invalid modules",
                   []() { s_validate = false; });
  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) { s_infile = argument; });

  parser.Parse(argc, argv);
}

Result write_ir_file(Module* module, const char* filename) {
  IRVisitor visitor;
  Result result = visitor.VisitModule(module);
  puts("Parsed module:");
  visitor.ir->dump();
  // TODO: actually write to file
  return result;
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
    Module* module = script->GetFirstModule();
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
