module;
#include <limits>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <utility>
#include <variant>
#include <vector>

module lyra.lowering;
import lyra.check;

namespace lyra {
namespace {

using Builder = llvm::IRBuilder<>;
using Value = llvm::Value*;
using Label = llvm::BasicBlock*;

struct Layouts {
    const CheckedModule& program;
    std::vector<llvm::StructType*> objects;

    Layouts(const CheckedModule& program, Builder& builder) : program(program) {
        for (const auto& record : program.aggregates) objects.push_back(llvm::StructType::create(builder.getContext(), "lyra.type." + record.syntax.name.text));
        for (std::size_t id = 0; id < objects.size(); ++id) {
            std::vector<llvm::Type*> fields;
            for (Type field : program.aggregates[id].fields) fields.push_back(native(builder, field));
            if (fields.empty() && program.aggregates[id].syntax.reference) fields.push_back(builder.getInt8Ty());
            objects[id]->setBody(fields);
        }
    }

    bool reference(Type type) const { return program.aggregates[type.id].syntax.reference; }

    llvm::Type* native(Builder& builder, Type type) const {
        switch (type.kind) {
        case Type::Int: return builder.getInt64Ty();
        case Type::Bool: return builder.getInt1Ty();
        case Type::Unit: case Type::Never: return builder.getInt8Ty();
        case Type::String: return builder.getPtrTy();
        case Type::Variable: llvm_unreachable("abstract type reached LLVM lowering");
        case Type::Record: return reference(type) ? static_cast<llvm::Type*>(builder.getPtrTy()) : objects[type.id];
        }
        llvm_unreachable("invalid Lyra type");
    }
};

struct Emitter {
    const CheckedFunction& fn;
    const Source& source;
    llvm::Module& module;
    llvm::Function* native;
    const std::vector<llvm::Function*>& functions;
    const Layouts& layouts;
    Builder builder{module.getContext()};
    std::vector<Value> locals{};
    std::vector<std::pair<Label, Label>> loops{};

    Label label(const char* name) { return llvm::BasicBlock::Create(module.getContext(), name, native); }
    bool live() const { return builder.GetInsertBlock() != nullptr; }
    Value unit() { return builder.getInt8(0); }
    void end() { builder.ClearInsertionPoint(); }

    void run() {
        builder.SetInsertPoint(label("entry"));
        for (auto local : fn.locals) locals.push_back(builder.CreateAlloca(layouts.native(builder, local.type)));
        for (auto& arg : native->args()) builder.CreateStore(&arg, locals[arg.getArgNo()]);
        auto tail = emit(fn.syntax.body);
        if (live()) builder.CreateRet(tail);
    }

    Value emit(ExprId id) {
        return std::visit([&](const auto& node) { return emitNode(id, node); }, fn.syntax.expressions[id].node);
    }

    Value emitNode(ExprId, std::int64_t value) { return builder.getInt64(value); }
    Value emitNode(ExprId, bool value) { return builder.getInt1(value); }
    Value emitNode(ExprId, const StringLiteral& string) {
        auto data = llvm::ConstantDataArray::getString(module.getContext(), string.bytes, false);
        auto constant = llvm::ConstantStruct::getAnon({builder.getInt64(string.bytes.size()), data});
        auto global = new llvm::GlobalVariable(module, constant->getType(), true, llvm::GlobalValue::PrivateLinkage, constant, "str");
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return global;
    }
    Value emitNode(ExprId id, const Identifier&) {
        return builder.CreateLoad(layouts.native(builder, fn.expressions[id].type), locals[fn.expressions[id].target]);
    }

    Value address(ExprId id) {
        const auto& node = fn.syntax.expressions[id].node;
        if (std::holds_alternative<Identifier>(node)) return locals[fn.expressions[id].target];
        const auto& member = std::get<Member>(node);
        auto owner = fn.expressions[member.receiver].type;
        auto base = layouts.reference(owner) ? emit(member.receiver) : address(member.receiver);
        if (!live()) return nullptr;
        return builder.CreateStructGEP(layouts.objects[owner.id], base, fn.expressions[id].target);
    }

    Value emitNode(ExprId id, const Member& member) {
        auto object = emit(member.receiver);
        if (!live()) return unit();
        auto owner = fn.expressions[member.receiver].type;
        auto slot = static_cast<unsigned>(fn.expressions[id].target);
        if (!layouts.reference(owner)) return builder.CreateExtractValue(object, slot);
        auto field = builder.CreateStructGEP(layouts.objects[owner.id], object, slot);
        return builder.CreateLoad(layouts.native(builder, fn.expressions[id].type), field);
    }

    Value emitNode(ExprId id, const Binding& binding) {
        auto value = emit(binding.value);
        if (live()) builder.CreateStore(value, locals[fn.expressions[id].target]);
        return unit();
    }

    Value emitNode(ExprId id, const Block& block) {
        Value tail = unit();
        for (const auto& statement : block.statements) {
            tail = emit(statement.expression);
            if (!live()) break;
            if (statement.semicolon) tail = unit();
        }
        return fn.expressions[id].type == Type::Unit ? unit() : tail;
    }

    Value emitNode(ExprId id, const Call& call) {
        const auto& info = fn.expressions[id];
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        std::vector<Value> args(call.arguments.size() + skip);
        if (call.receiver) {
            args[0] = emit(*call.receiver);
            if (!live()) return unit();
        }
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            args[info.argumentSlots[index] + skip] = emit(call.arguments[index].value);
            if (!live()) return unit();
        }
        switch (info.callee) {
            case Callee::Print: {
                bool string = fn.expressions[call.arguments[0].value].type == Type::String;
                builder.CreateCall(module.getOrInsertFunction(string ? "lyraPrintString" : "lyraPrintInt", builder.getVoidTy(), args[0]->getType()), args);
                return unit();
            }
            case Callee::Collect:
                builder.CreateCall(module.getOrInsertFunction("lyraGcCollect", builder.getVoidTy()));
                return unit();
            case Callee::Constructor: {
                auto storage = layouts.objects[info.target];
                if (layouts.program.aggregates[info.target].syntax.reference) {
                    auto alloc = module.getOrInsertFunction("lyraGcAlloc", builder.getPtrTy(), builder.getInt64Ty());
                    auto object = builder.CreateCall(alloc, {llvm::ConstantExpr::getSizeOf(storage)});
                    for (unsigned slot = 0; slot < args.size(); ++slot) builder.CreateStore(args[slot], builder.CreateStructGEP(storage, object, slot));
                    return object;
                }
                Value value = llvm::Constant::getNullValue(storage);
                for (unsigned slot = 0; slot < args.size(); ++slot) value = builder.CreateInsertValue(value, args[slot], slot);
                return value;
            }
            case Callee::Function: return builder.CreateCall(functions[info.target], args);
        }
        llvm_unreachable("invalid Lyra callee");
    }

    void trapIf(Value condition, Span site, const char* message) {
        auto failed = label("arithmetic.error"), next = label("arithmetic.ok");
        builder.CreateCondBr(condition, failed, next);
        builder.SetInsertPoint(failed);
        auto pointer = builder.getPtrTy();
        auto integer = builder.getInt64Ty();
        auto error = module.getOrInsertFunction("lyraArithmeticError", builder.getVoidTy(), pointer, integer, integer, pointer);
        auto position = source.position(site.begin);
        builder.CreateCall(error, {builder.CreateGlobalStringPtr(source.path.string()), builder.getInt64(position.line),
                                   builder.getInt64(position.column), builder.CreateGlobalStringPtr(message)});
        builder.CreateUnreachable();
        builder.SetInsertPoint(next);
    }

    Value arithmetic(Op op, Value left, Value right, Span site) {
        if (op == Op::Div || op == Op::Rem) {
            trapIf(builder.CreateICmpEQ(right, builder.getInt64(0)), site, "Int division by zero");
            auto minusOne = builder.CreateICmpEQ(right, builder.getInt64(-1));
            if (op == Op::Rem) return builder.CreateSRem(left, builder.CreateSelect(minusOne, builder.getInt64(1), right));
            auto minimum = builder.getInt64(std::numeric_limits<std::int64_t>::min());
            trapIf(builder.CreateAnd(minusOne, builder.CreateICmpEQ(left, minimum)), site, "Int overflow (/)");
            return builder.CreateSDiv(left, right);
        }
        auto intrinsic = op == Op::Add ? llvm::Intrinsic::sadd_with_overflow : op == Op::Sub ? llvm::Intrinsic::ssub_with_overflow : llvm::Intrinsic::smul_with_overflow;
        auto checked = llvm::Intrinsic::getDeclaration(&module, intrinsic, {builder.getInt64Ty()});
        auto pair = builder.CreateCall(checked, {left, right});
        trapIf(builder.CreateExtractValue(pair, 1), site, op == Op::Add ? "Int overflow (+)" : op == Op::Sub ? "Int overflow (-)" : "Int overflow (*)");
        return builder.CreateExtractValue(pair, 0);
    }

    Value emitNode(ExprId, const Unary& unary) {
        auto value = emit(unary.operand);
        if (!live()) return unit();
        return unary.op == Op::Not ? builder.CreateNot(value) : arithmetic(Op::Sub, builder.getInt64(0), value, unary.site);
    }

    Value emitNode(ExprId, const Binary& binary) {
        auto op = binary.op;
        if (op <= Op::RemSet) {
            if (fn.expressions[binary.left].type == Type::Never) return emit(binary.left);
            auto slot = address(binary.left);
            if (!live()) return unit();
            auto type = fn.expressions[binary.left].type;
            auto left = op == Op::Set ? nullptr : builder.CreateLoad(layouts.native(builder, type), slot);
            auto right = emit(binary.right);
            if (!live()) return unit();
            constexpr Op compound[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Rem};
            if (op != Op::Set) {
                right = type == Type::String ? concatenate(left, right) : arithmetic(compound[static_cast<int>(op) - 1], left, right, binary.site);
            }
            builder.CreateStore(right, slot);
            return unit();
        }
        auto left = emit(binary.left);
        if (!live()) return unit();
        if (op == Op::And || op == Op::Or) {
            auto from = builder.GetInsertBlock(), rhs = label("rhs"), join = label("logic.end");
            builder.CreateCondBr(left, op == Op::And ? rhs : join, op == Op::And ? join : rhs);
            builder.SetInsertPoint(rhs);
            auto right = emit(binary.right);
            auto last = builder.GetInsertBlock();
            if (last) builder.CreateBr(join);
            builder.SetInsertPoint(join);
            auto value = builder.CreatePHI(builder.getInt1Ty(), 2);
            value->addIncoming(left, from);
            if (last) value->addIncoming(right, last);
            return value;
        }
        auto right = emit(binary.right);
        if (!live()) return unit();
        if (fn.expressions[binary.left].type == Type::String) {
            if (op == Op::Add) return concatenate(left, right);
            auto pointer = builder.getPtrTy();
            auto equal = builder.CreateCall(module.getOrInsertFunction("lyraStringEqual", builder.getInt8Ty(), pointer, pointer), {left, right});
            return op == Op::Eq ? builder.CreateIsNotNull(equal) : builder.CreateIsNull(equal);
        }
        switch (op) {
            case Op::Eq: return builder.CreateICmpEQ(left, right);
            case Op::Ne: return builder.CreateICmpNE(left, right);
            case Op::Lt: return builder.CreateICmpSLT(left, right);
            case Op::Le: return builder.CreateICmpSLE(left, right);
            case Op::Gt: return builder.CreateICmpSGT(left, right);
            case Op::Ge: return builder.CreateICmpSGE(left, right);
            default: return arithmetic(op, left, right, binary.site);
        }
    }

    Value concatenate(Value left, Value right) {
        auto pointer = builder.getPtrTy();
        return builder.CreateCall(module.getOrInsertFunction("lyraStringConcat", pointer, pointer, pointer), {left, right});
    }

    Value emitNode(ExprId id, const Conditional& branch) {
        auto condition = emit(branch.condition);
        if (!live()) return unit();
        auto yes = label("then"), no = label("else"), join = label("if.end");
        builder.CreateCondBr(condition, yes, no);
        builder.SetInsertPoint(yes);
        auto trueValue = emit(branch.yes);
        auto trueEnd = builder.GetInsertBlock();
        if (trueEnd) builder.CreateBr(join);
        builder.SetInsertPoint(no);
        auto falseValue = branch.no ? emit(*branch.no) : unit();
        auto falseEnd = builder.GetInsertBlock();
        if (falseEnd) builder.CreateBr(join);
        if (!trueEnd && !falseEnd) { join->eraseFromParent(); end(); return unit(); }
        builder.SetInsertPoint(join);
        auto type = fn.expressions[id].type;
        if (type == Type::Unit) return unit();
        auto value = builder.CreatePHI(layouts.native(builder, type), 2);
        if (trueEnd) value->addIncoming(trueValue, trueEnd);
        if (falseEnd) value->addIncoming(falseValue, falseEnd);
        return value;
    }

    Value emitNode(ExprId, const While& loop) {
        auto test = label("while.test");
        builder.CreateBr(test);
        builder.SetInsertPoint(test);
        auto condition = emit(loop.condition);
        if (!live()) return unit();
        auto body = label("while.body"), done = label("while.end");
        builder.CreateCondBr(condition, body, done);
        builder.SetInsertPoint(body);
        loops.emplace_back(test, done);
        emit(loop.body);
        loops.pop_back();
        if (live()) builder.CreateBr(test);
        builder.SetInsertPoint(done);
        return unit();
    }

    Value emitNode(ExprId, const Return& ret) {
        auto value = ret.value ? emit(*ret.value) : unit();
        if (live()) builder.CreateRet(value);
        end();
        return unit();
    }

    Value emitNode(ExprId, Jump jump) {
        auto [test, done] = loops.back();
        builder.CreateBr(jump == Jump::Break ? done : test);
        end();
        return unit();
    }
};

}

std::unique_ptr<llvm::Module> lower(const CheckedModule& program, const std::vector<Source>& sources, llvm::LLVMContext& context) {
    auto module = std::make_unique<llvm::Module>("lyra", context);
    module->setTargetTriple("x86_64-pc-linux-gnu");
    Builder builder(context);
    Layouts layouts(program, builder);
    std::vector<llvm::Function*> functions;
    for (const auto& fn : program.functions) {
        std::vector<llvm::Type*> params;
        for (std::size_t index = 0; index < fn.syntax.parameters.size(); ++index) params.push_back(layouts.native(builder, fn.locals[index].type));
        auto signature = llvm::FunctionType::get(layouts.native(builder, fn.result), params, false);
        functions.push_back(llvm::Function::Create(signature, llvm::Function::InternalLinkage, "lyra.fn." + fn.syntax.name.text, *module));
    }
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const auto& fn = program.functions[index];
        Emitter{fn, sources[fn.syntax.file], *module, functions[index], functions, layouts}.run();
    }
    auto main = llvm::Function::Create(llvm::FunctionType::get(builder.getInt32Ty(), false), llvm::Function::ExternalLinkage, "main", *module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
    builder.CreateCall(functions[program.entry.value()]);
    builder.CreateRet(builder.getInt32(0));
    std::string reason;
    llvm::raw_string_ostream log(reason);
    if (llvm::verifyModule(*module, &log)) throw Diagnostic{"LYB0001", "编译器生成的 LLVM 模块无效：" + reason};
    return module;
}

}
