module;
#include <algorithm>
#include <cmath>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <utility>
#include <variant>
#include <vector>

module lyra.lowering;
import lyra.check;
import lyra.async;
import lyra.foreign;
import lyra.collections;
import lyra.closures;
import lyra.exceptions;
import lyra.numbers;
import lyra.platform;

namespace lyra {
namespace {

using Builder = llvm::IRBuilder<>;
using Value = llvm::Value*;
using Label = llvm::BasicBlock*;

struct Layouts {
    const CheckedModule& program;
    std::vector<llvm::StructType*> objects;
    std::vector<std::vector<llvm::StructType*>> payloads;
    bool workers{};

    Layouts(const CheckedModule& program, Builder& builder) : program(program) {
        for (const auto& fn : program.functions) workers |= std::ranges::any_of(fn.syntax.expressions, [](const Expr& expr) { return std::holds_alternative<Blocking>(expr.node); });
        for (const auto& record : program.aggregates) objects.push_back(record.abstract ? nullptr : llvm::StructType::create(builder.getContext(), "lyra.type." + record.syntax.name.text));
        for (std::size_t id = 0; id < objects.size(); ++id) {
            if (program.aggregates[id].abstract) continue;
            std::vector<llvm::Type*> fields;
            for (Type field : program.aggregates[id].fields) fields.push_back(native(builder, field));
            if (fields.empty() && program.aggregates[id].syntax.reference) fields.push_back(builder.getInt8Ty());
            objects[id]->setBody(fields);
        }
        for (const auto& item : program.enums) {
            auto& cases = payloads.emplace_back();
            for (const auto& payload : item.payloads) {
                std::vector<llvm::Type*> fields;
                for (auto field : payload) fields.push_back(native(builder, field));
                cases.push_back(llvm::StructType::get(builder.getContext(), fields));
            }
        }
    }

    bool reference(Type type) const { return program.aggregates[type.id].syntax.reference; }

    llvm::Type* native(Builder& builder, Type type) const {
        switch (type.kind) {
        case Type::Int: case Type::UInt: return builder.getIntNTy(numberWidth(type));
        case Type::Real: return numberWidth(type) == 32 ? builder.getFloatTy() : builder.getDoubleTy();
        case Type::Bool: return builder.getInt1Ty();
        case Type::Char: return builder.getInt32Ty();
        case Type::Unit: case Type::Never: return builder.getInt8Ty();
        case Type::String: case Type::Bytes: case Type::Task: case Type::CPtr: case Type::List: case Type::Exception: case Type::Closure: return builder.getPtrTy();
        case Type::Variable: llvm_unreachable("abstract type reached LLVM lowering");
        case Type::Record: return reference(type) ? static_cast<llvm::Type*>(builder.getPtrTy()) : objects[type.id];
        case Type::Enum: return llvm::StructType::get(builder.getContext(), {builder.getInt32Ty(), builder.getPtrTy()});
        case Type::Option: return llvm::StructType::get(builder.getContext(), {builder.getInt1Ty(), native(builder, program.sumTypes[type.id].success)});
        case Type::Result: return llvm::StructType::get(builder.getContext(), {builder.getInt1Ty(), native(builder, program.sumTypes[type.id].success), native(builder, program.sumTypes[type.id].failure)});
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
    struct Loop { Label test, done; Value cleanups; std::size_t handlers; };
    std::vector<Loop> loops{};
    std::optional<ExprId> worker{};
    llvm::AllocaInst* cleanupHead{};
    llvm::AllocaInst* errorSlot{};
    struct Handler { Label landing; Value mark; };
    std::vector<Handler> handlers{};
    std::vector<Value> caught{};

    Label label(const char* name) { return llvm::BasicBlock::Create(module.getContext(), name, native); }
    bool live() const { return builder.GetInsertBlock() != nullptr; }
    Value unit() { return builder.getInt8(0); }
    void end() { builder.ClearInsertionPoint(); }

    bool deferred() const { return worker && std::holds_alternative<Deferred>(fn.syntax.expressions[*worker].node); }
    bool lambda() const { return worker && std::holds_alternative<Lambda>(fn.syntax.expressions[*worker].node); }
    ExprId bodyId() const {
        if (!worker) return fn.syntax.body;
        const auto& node = fn.syntax.expressions[*worker].node;
        if (auto cleanup = std::get_if<Deferred>(&node)) return cleanup->body;
        if (auto literal = std::get_if<Lambda>(&node)) return literal->body;
        return std::get<Blocking>(node).body;
    }
    Type completionType() const {
        if (!worker) return fn.result;
        if (lambda()) return layouts.program.signatures[rawType(*worker).id].result;
        return deferred() ? Type{Type::Unit} : layouts.program.taskResults[rawType(*worker).id];
    }
    Value cleanupMark() { return cleanupHead ? builder.CreateLoad(builder.getPtrTy(), cleanupHead) : nullptr; }
    bool asyncBody() const { return !worker && fn.syntax.isAsync; }

    void propagate(Value error, std::size_t depth) {
        if (depth) {
            const auto& handler = handlers[depth - 1];
            if (cleanupHead) error = cleanupUntil(builder, cleanupHead, handler.mark, error);
            builder.CreateStore(error, errorSlot);
            builder.CreateBr(handler.landing);
        } else if (asyncBody()) {
            asyncFailure(builder, error);
            builder.CreateUnreachable();
        } else {
            if (cleanupHead) error = cleanupUntil(builder, cleanupHead, nullptr, error);
            builder.CreateStore(error, native->getArg(native->arg_size() - 1));
            builder.CreateRet(llvm::Constant::getNullValue(native->getReturnType()));
        }
        end();
    }

    void guard(Value error, std::size_t depth, Span site = {}, const char* role = nullptr) {
        auto failed = label("exception"), normal = label("no.exception");
        builder.CreateCondBr(builder.CreateIsNotNull(error), failed, normal);
        builder.SetInsertPoint(failed);
        if (role) error = NativeExceptions{builder}.locate(error, source, site, role);
        propagate(error, depth);
        builder.SetInsertPoint(normal);
    }

    void unwind(Value mark, std::size_t depth) {
        if (cleanupHead) guard(cleanupUntil(builder, cleanupHead, mark), depth);
    }

    void exitHandlers(std::size_t remain) {
        for (auto depth = handlers.size(); depth > remain; --depth) unwind(handlers[depth - 1].mark, depth);
    }

    void leave(Value value) {
        exitHandlers(0);
        if (!asyncBody()) unwind(nullptr, 0);
        builder.CreateRet(value);
    }

    Value callFunction(std::size_t target, llvm::ArrayRef<Value> args, Span site) {
        const auto& syntax = layouts.program.functions[target].syntax;
        if (syntax.foreignSymbol) return builder.CreateCall(functions[target], args);
        if (syntax.isAsync) return NativeExceptions{builder}.created(builder.CreateCall(functions[target], args), source, site);
        std::vector<Value> nativeArgs(args.begin(), args.end());
        nativeArgs.push_back(errorSlot);
        auto result = builder.CreateCall(functions[target], nativeArgs);
        guard(builder.CreateLoad(builder.getPtrTy(), errorSlot), handlers.size(), site, "call");
        return result;
    }

    Value cell(std::size_t slot) {
        return fn.locals[slot].captured ? builder.CreateLoad(builder.getPtrTy(), locals[slot]) : locals[slot];
    }

    Value read(std::size_t slot) {
        auto* type = layouts.native(builder, fn.locals[slot].type);
        auto address = cell(slot);
        return fn.locals[slot].captured && layouts.workers ? sharedRead(builder, type, address) : builder.CreateLoad(type, address);
    }

    llvm::FunctionType* signature(const FunctionShape& shape) {
        std::vector<llvm::Type*> parameters;
        for (auto type : shape.parameters) parameters.push_back(layouts.native(builder, type));
        return NativeClosures{builder}.signature(layouts.native(builder, shape.result), parameters, shape.result.kind == Type::Task);
    }

    void initialize(std::size_t slot, Value value) {
        if (fn.locals[slot].captured) {
            auto address = asyncCell(builder, value->getType());
            builder.CreateStore(value, address);
            builder.CreateStore(address, locals[slot]);
        } else builder.CreateStore(value, locals[slot]);
    }

    void run(std::optional<ExprId> workerId = {}) {
        worker = workerId;
        builder.SetInsertPoint(label("entry"));
        errorSlot = NativeExceptions{builder}.slot();
        if (std::ranges::find(fn.cleanupOwners, worker) != fn.cleanupOwners.end()) cleanupHead = cleanupStack(builder);
        locals.resize(fn.locals.size());
        for (std::size_t slot = 0; slot < fn.locals.size(); ++slot) {
            const auto& local = fn.locals[slot];
            if (local.worker == worker) locals[slot] = builder.CreateAlloca(local.captured ? builder.getPtrTy() : layouts.native(builder, local.type));
        }
        if (lambda()) {
            const auto& literal = fn.expressions[*worker];
            for (std::size_t index = 0; index < literal.captures.size(); ++index) {
                auto slot = literal.captures[index];
                locals[slot] = builder.CreateAlloca(builder.getPtrTy());
                builder.CreateStore(NativeClosures{builder}.capture(native->getArg(0), literal.captures.size(), index), locals[slot]);
            }
            for (std::size_t index = 0; index < literal.argumentSlots.size(); ++index) initialize(literal.argumentSlots[index], native->getArg(index + 1));
        } else for (auto& arg : native->args()) {
            if (!asyncBody() && arg.getArgNo() + 1 == native->arg_size()) break;
            if (worker) {
                auto slot = fn.expressions[*worker].captures[arg.getArgNo()];
                locals[slot] = builder.CreateAlloca(builder.getPtrTy());
                builder.CreateStore(&arg, locals[slot]);
            } else initialize(arg.getArgNo(), &arg);
        }
        if (initializer()) initialize(fn.syntax.parameters.size(), asyncCell(builder, layouts.objects[fn.result.id]));
        auto tail = emit(bodyId());
        if (live()) leave(initializer() ? read(fn.syntax.parameters.size()) : deferred() ? unit() : tail);
    }

    bool initializer() const { return !worker && fn.syntax.initializer; }

    Type rawType(ExprId id) const { const auto& info = fn.expressions[id]; return info.sourceType.value_or(info.type); }

    Value makeSum(Type type, SumTag tag, Value payload = nullptr) {
        Value value = llvm::Constant::getNullValue(layouts.native(builder, type));
        value = builder.CreateInsertValue(value, builder.getInt1(successTag(tag)), 0);
        return payload ? builder.CreateInsertValue(value, payload, payloadSlot(tag)) : value;
    }

    Value makeVariant(ExprId id, llvm::ArrayRef<Value> values = {}) {
        auto type = rawType(id);
        auto index = fn.expressions[id].target;
        auto tag = builder.getInt32(static_cast<unsigned>(index));
        Value result = builder.CreateInsertValue(llvm::Constant::getNullValue(layouts.native(builder, type)), tag, 0);
        if (values.empty()) return result;
        auto storage = layouts.payloads[type.id][index];
        auto payload = asyncCell(builder, storage);
        for (unsigned slot = 0; slot < values.size(); ++slot) builder.CreateStore(values[slot], builder.CreateStructGEP(storage, payload, slot));
        return builder.CreateInsertValue(result, payload, 1);
    }

    Value castNumber(Value value, Type from, Type to) {
        auto* target = layouts.native(builder, to);
        if (from == to) return value;
        if (to.kind != Type::Real) return builder.CreateIntCast(value, target, from.kind == Type::Int);
        if (from.kind == Type::Real) return builder.CreateFPCast(value, target);
        return from.kind == Type::Int ? builder.CreateSIToFP(value, target) : builder.CreateUIToFP(value, target);
    }

    Value checkedInteger(Value value, Type from, Type option) {
        auto to = layouts.program.sumTypes[option.id].success;
        if (losslessNumber(from, to)) return makeSum(option, SumTag::Some, castNumber(value, from, to));
        auto* target = layouts.native(builder, to);
        auto zero = llvm::Constant::getNullValue(target);
        auto optional = [&](Value present, Value payload) {
            auto selected = builder.CreateSelect(present, payload, zero);
            return builder.CreateInsertValue(makeSum(option, SumTag::None, selected), present, 0);
        };
        if (integerType(from)) {
            auto bits = numberWidth(to);
            auto width = std::max(numberWidth(from), bits) + 1;
            auto* wideType = builder.getIntNTy(width);
            auto wide = builder.CreateIntCast(value, wideType, from.kind == Type::Int);
            auto low = to.kind == Type::Int ? llvm::APInt::getSignedMinValue(bits).sext(width) : llvm::APInt(width, 0);
            auto high = to.kind == Type::Int ? llvm::APInt::getSignedMaxValue(bits).sext(width) : llvm::APInt::getMaxValue(bits).zext(width);
            auto fits = builder.CreateAnd(builder.CreateICmpSGE(wide, llvm::ConstantInt::get(wideType, low)),
                                          builder.CreateICmpSLE(wide, llvm::ConstantInt::get(wideType, high)));
            return optional(fits, castNumber(value, from, to));
        }
        auto limit = std::ldexp(1.0, static_cast<int>(numberWidth(to)) - (to.kind == Type::Int));
        auto low = llvm::ConstantFP::get(value->getType(), to.kind == Type::Int ? -limit : 0.0);
        auto high = llvm::ConstantFP::get(value->getType(), limit);
        auto fits = builder.CreateAnd(builder.CreateFCmpOGE(value, low), builder.CreateFCmpOLT(value, high));
        auto entry = builder.GetInsertBlock();
        auto convert = label("numeric.convert"), done = label("numeric.end");
        builder.CreateCondBr(fits, convert, done);
        builder.SetInsertPoint(convert);
        auto converted = to.kind == Type::Int ? builder.CreateFPToSI(value, target) : builder.CreateFPToUI(value, target);
        auto exact = builder.CreateFCmpOEQ(value, castNumber(converted, to, from));
        auto result = optional(exact, converted);
        builder.CreateBr(done);
        builder.SetInsertPoint(done);
        auto merged = builder.CreatePHI(layouts.native(builder, option), 2);
        merged->addIncoming(makeSum(option, SumTag::None), entry);
        merged->addIncoming(result, convert);
        return merged;
    }

    Value emit(ExprId id) {
        auto value = std::visit([&](const auto& node) { return emitNode(id, node); }, fn.syntax.expressions[id].node);
        const auto& info = fn.expressions[id];
        if (!live() || !info.sourceType) return value;
        if (info.conversion) return callFunction(*info.conversion, {value}, fn.syntax.expressions[id].span);
        if (info.type.kind == Type::Option) return makeSum(info.type, SumTag::Some, value);
        return castNumber(value, *info.sourceType, info.type);
    }

    Value emitNode(ExprId id, IntegerLiteral literal) {
        auto* type = layouts.native(builder, rawType(id));
        if (auto value = fn.expressions[id].realLiteral) return llvm::ConstantFP::get(type, *value);
        return llvm::ConstantInt::get(type, integerBits(literal));
    }
    Value emitNode(ExprId id, const RealLiteral&) {
        return llvm::ConstantFP::get(layouts.native(builder, rawType(id)), *fn.expressions[id].realLiteral);
    }
    Value emitNode(ExprId, bool value) { return builder.getInt1(value); }
    Value emitNode(ExprId, CharLiteral value) { return builder.getInt32(value.point); }
    Value stringConstant(llvm::StringRef bytes) {
        auto data = llvm::ConstantDataArray::getString(module.getContext(), bytes, false);
        auto constant = llvm::ConstantStruct::getAnon({builder.getInt64(bytes.size()), data});
        auto global = new llvm::GlobalVariable(module, constant->getType(), true, llvm::GlobalValue::PrivateLinkage, constant, "str");
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return global;
    }
    Value emitNode(ExprId, const StringLiteral& string) { return stringConstant(string.bytes); }
    Value emitNode(ExprId, const BytesLiteral& literal) { return stringConstant(literal.bytes); }

    Value displayScalar(Value value, Type type) {
        const char* symbol;
        switch (type.kind) {
            case Type::String: return value;
            case Type::Bool: return builder.CreateSelect(value, stringConstant("true"), stringConstant("false"));
            case Type::Char: symbol = "lyraCharText"; break;
            case Type::Int: case Type::UInt:
                symbol = type.kind == Type::Int ? "lyraIntText" : "lyraUIntText";
                value = castNumber(value, type, Type{type.kind});
                break;
            case Type::Real: symbol = numberWidth(type) == 32 ? "lyraFloat32Text" : "lyraFloat64Text"; break;
            default: llvm_unreachable("value requires a Display method");
        }
        return builder.CreateCall(module.getOrInsertFunction(symbol, builder.getPtrTy(), value->getType()), {value});
    }

    Value emitNode(ExprId id, const DisplayValue& display) {
        auto value = emit(display.value);
        if (!live()) return unit();
        auto type = fn.expressions[display.value].type;
        if (type.kind == Type::Record) return callFunction(fn.expressions[id].target, {value}, fn.syntax.expressions[display.value].span);
        return displayScalar(value, type);
    }

    Value emitNode(ExprId, const TextTemplate& text) {
        if (text.parts.size() == 1) return emit(text.parts.front());
        auto pointer = builder.getPtrTy();
        auto array = llvm::ArrayType::get(pointer, text.parts.size());
        Builder entry(&native->getEntryBlock(), native->getEntryBlock().getFirstInsertionPt());
        auto parts = entry.CreateAlloca(array, nullptr, "text.parts");
        for (std::size_t index = 0; index < text.parts.size(); ++index) {
            auto value = emit(text.parts[index]);
            if (!live()) return unit();
            builder.CreateStore(value, builder.CreateInBoundsGEP(array, parts, {builder.getInt64(0), builder.getInt64(index)}));
        }
        auto join = module.getOrInsertFunction("lyraStringJoin", pointer, builder.getInt64Ty(), pointer);
        auto result = builder.CreateCall(join, {builder.getInt64(text.parts.size()), parts});
        builder.CreateStore(llvm::Constant::getNullValue(array), parts);
        return result;
    }

    Value emitNode(ExprId id, const Identifier&) {
        const auto& info = fn.expressions[id];
        if (info.functionRef) {
            const auto& syntax = layouts.program.functions[*info.functionRef].syntax;
            const auto& shape = layouts.program.signatures[rawType(id).id];
            return NativeClosures{builder}.named(functions[*info.functionRef], signature(shape), shape.parameters.size(), syntax.isAsync, syntax.foreignSymbol.has_value());
        }
        return read(info.target);
    }

    Value emitNode(ExprId id, This) { return read(fn.expressions[id].target); }

    Value emitNode(ExprId id, const Lambda&) {
        NativeClosures closures{builder};
        auto* code = closures.function(signature(layouts.program.signatures[rawType(id).id]), native->getName() + ".lambda");
        Emitter{fn, source, module, code, functions, layouts}.run(id);
        std::vector<Value> cells;
        for (auto slot : fn.expressions[id].captures) cells.push_back(cell(slot));
        return closures.close(code, cells);
    }

    Value emitNode(ExprId id, const TaskOp& op) {
        auto task = emit(op.operand);
        if (!live()) return unit();
        if (op.spawn) return asyncSpawn(builder, task);
        auto result = asyncAwait(builder, layouts.native(builder, rawType(id)), task, errorSlot);
        guard(builder.CreateLoad(builder.getPtrTy(), errorSlot), handlers.size(), op.site, "await");
        return result;
    }

    std::pair<BlockingCode, std::vector<Value>> closure(ExprId id, Type result, llvm::StringRef suffix) {
        const auto& captures = fn.expressions[id].captures;
        auto code = blockingCode(*native, layouts.native(builder, result), captures.size(), suffix);
        Emitter{fn, source, module, code.body, functions, layouts}.run(id);
        std::vector<Value> cells;
        for (auto slot : captures) cells.push_back(cell(slot));
        return {code, std::move(cells)};
    }

    Value emitNode(ExprId id, const Blocking&) {
        auto [code, cells] = closure(id, layouts.program.taskResults[rawType(id).id], ".worker");
        return NativeExceptions{builder}.created(blockingTask(builder, code, cells), source, fn.syntax.expressions[id].span);
    }

    Value emitNode(ExprId id, const Deferred&) {
        auto [code, cells] = closure(id, Type::Unit, ".defer");
        pushCleanup(builder, cleanupHead, code, cells);
        return unit();
    }

    Value emitNode(ExprId id, const EmptyOption&) { return makeSum(rawType(id), SumTag::None); }

    Value emitNode(ExprId, const ListLiteral& literal) {
        NativeLists lists{builder};
        auto result = lists.create();
        for (auto element : literal.elements) {
            auto value = emit(element);
            if (!live()) return unit();
            lists.add(result, value);
        }
        return result;
    }

    Value emitNode(ExprId id, const Index& index) {
        auto receiver = emit(index.receiver);
        if (!live()) return unit();
        auto offset = emit(index.offset);
        if (!live()) return unit();
        auto* result = layouts.native(builder, rawType(id));
        auto owner = fn.expressions[index.receiver].type;
        if (owner.kind == Type::List) return builder.CreateLoad(result, NativeLists{builder}.get(receiver, offset, source, index.site));
        auto site = source.origin ? *source.origin : source.site(index.site);
        auto pointer = builder.getPtrTy(); auto integer = builder.getInt64Ty();
        auto access = module.getOrInsertFunction(owner == Type::String ? "lyraStringIndex" : "lyraBytesIndex", result, pointer, integer, pointer, integer, integer);
        return builder.CreateCall(access, {receiver, offset, builder.CreateGlobalStringPtr(site.file), builder.getInt64(site.start.line), builder.getInt64(site.start.column)});
    }

    Value emitNode(ExprId id, const Propagate& propagation) {
        auto value = emit(propagation.operand);
        if (!live()) return unit();
        auto success = label("propagate.value"), failure = label("propagate.return");
        builder.CreateCondBr(builder.CreateExtractValue(value, 0), success, failure);
        builder.SetInsertPoint(failure);
        auto target = completionType();
        Value result = nullptr;
        if (target.kind == Type::Result) {
            result = builder.CreateExtractValue(value, payloadSlot(SumTag::Err));
            if (auto conversion = fn.expressions[id].errorConversion) result = callFunction(*conversion, {result}, propagation.site);
        }
        leave(makeSum(target, target.kind == Type::Option ? SumTag::None : SumTag::Err, result));
        builder.SetInsertPoint(success);
        return builder.CreateExtractValue(value, 1);
    }

    Value stringEqual(Value left, Value right) {
        auto pointer = builder.getPtrTy();
        auto equal = builder.CreateCall(module.getOrInsertFunction("lyraStringEqual", builder.getInt8Ty(), pointer, pointer), {left, right});
        return builder.CreateIsNotNull(equal);
    }

    void matchTest(Value passes, Label miss) {
        auto matched = label("match.pattern");
        builder.CreateCondBr(passes, matched, miss);
        builder.SetInsertPoint(matched);
    }

    void emitPattern(const CheckedPattern& pattern, Value subject, Label miss, std::vector<std::pair<std::size_t, Value>>& bindings) {
        if (auto* slot = std::get_if<PatternSlot>(&pattern.node)) bindings.emplace_back(slot->local, subject);
        else if (auto* number = std::get_if<std::uint64_t>(&pattern.node))
            matchTest(builder.CreateICmpEQ(subject, llvm::ConstantInt::get(subject->getType(), *number)), miss);
        else if (auto* text = std::get_if<StringLiteral>(&pattern.node)) matchTest(stringEqual(subject, stringConstant(text->bytes)), miss);
        else if (auto* constructor = std::get_if<PatternConstructor>(&pattern.node)) {
            if (auto* tag = std::get_if<SumTag>(constructor)) {
                auto success = builder.CreateExtractValue(subject, 0);
                matchTest(successTag(*tag) ? success : builder.CreateNot(success), miss);
                if (*tag != SumTag::None) emitPattern(pattern.parts.front(), builder.CreateExtractValue(subject, payloadSlot(*tag)), miss, bindings);
            } else if (auto* variant = std::get_if<VariantTag>(constructor)) {
                matchTest(builder.CreateICmpEQ(builder.CreateExtractValue(subject, 0), builder.getInt32(variant->index)), miss);
                auto payload = builder.CreateExtractValue(subject, 1);
                auto storage = layouts.payloads[variant->owner][variant->index];
                for (unsigned slot = 0; slot < pattern.parts.size(); ++slot) {
                    const auto& part = pattern.parts[slot];
                    if (std::holds_alternative<std::monostate>(part.node)) continue;
                    auto value = builder.CreateLoad(storage->getElementType(slot), builder.CreateStructGEP(storage, payload, slot));
                    emitPattern(part, value, miss, bindings);
                }
            } else if (auto* flag = std::get_if<bool>(constructor)) matchTest(*flag ? subject : builder.CreateNot(subject), miss);
            else for (unsigned slot = 0; slot < pattern.parts.size(); ++slot) {
                const auto& part = pattern.parts[slot];
                if (!std::holds_alternative<std::monostate>(part.node)) emitPattern(part, builder.CreateExtractValue(subject, slot), miss, bindings);
            }
        }
    }

    Value emitNode(ExprId id, const Match& match) {
        auto subject = emit(match.subject);
        if (!live()) return unit();
        auto done = label("match.end");
        std::vector<std::pair<Value, Label>> values;
        for (std::size_t index = 0; index < match.arms.size(); ++index) {
            const auto& arm = match.arms[index];
            auto next = label("match.next");
            std::vector<std::pair<std::size_t, Value>> bindings;
            emitPattern(fn.expressions[id].patterns[index], subject, next, bindings);
            for (auto [slot, payload] : bindings) initialize(slot, payload);
            if (arm.guard) {
                auto condition = emit(*arm.guard);
                if (live()) matchTest(condition, next);
            }
            if (live()) {
                auto value = emit(arm.value);
                if (live()) { values.emplace_back(value, builder.GetInsertBlock()); builder.CreateBr(done); }
            }
            builder.SetInsertPoint(next);
        }
        builder.CreateUnreachable();
        if (values.empty()) { done->eraseFromParent(); end(); return unit(); }
        builder.SetInsertPoint(done);
        auto result = rawType(id);
        if (result == Type::Unit) return unit();
        auto value = builder.CreatePHI(layouts.native(builder, result), values.size());
        for (auto [incoming, block] : values) value->addIncoming(incoming, block);
        return value;
    }

    struct Place {
        Value pointer{}, list{}, index{};
        Type element{};
        Span site{};
        std::vector<unsigned> path{};
    };

    Place address(ExprId id) {
        const auto& node = fn.syntax.expressions[id].node;
        if (std::holds_alternative<Identifier>(node) || std::holds_alternative<This>(node)) return {cell(fn.expressions[id].target)};
        if (auto indexed = std::get_if<Index>(&node)) {
            auto list = emit(indexed->receiver);
            if (!live()) return {};
            auto offset = emit(indexed->offset);
            if (!live()) return {};
            return {NativeLists{builder}.get(list, offset, source, indexed->site), list, offset, rawType(id), indexed->site};
        }
        const auto& member = std::get<Member>(node);
        auto owner = fn.expressions[member.receiver].type;
        auto place = layouts.reference(owner) ? Place{emit(member.receiver)} : address(member.receiver);
        if (!live()) return {};
        auto slot = static_cast<unsigned>(fn.expressions[id].target);
        place.pointer = builder.CreateStructGEP(layouts.objects[owner.id], place.pointer, slot);
        if (place.list) place.path.push_back(slot);
        return place;
    }

    Value emitNode(ExprId id, const Member& member) {
        if (fn.expressions[id].callee == Callee::Variant) return makeVariant(id);
        auto object = emit(member.receiver);
        if (!live()) return unit();
        auto owner = fn.expressions[member.receiver].type;
        if (owner.kind == Type::List) return NativeLists{builder}.size(object);
        if (owner == Type::Char) return object;
        if (owner == Type::String || owner == Type::Bytes) {
            auto symbol = owner == Type::String && member.name.text == "size" ? "lyraStringScalarCount" : "lyraStringLength";
            return builder.CreateCall(module.getOrInsertFunction(symbol, builder.getInt64Ty(), builder.getPtrTy()), {object});
        }
        if (owner == Type::Exception) return NativeExceptions{builder}.message(object);
        auto slot = static_cast<unsigned>(fn.expressions[id].target);
        if (!layouts.reference(owner)) return builder.CreateExtractValue(object, slot);
        auto field = builder.CreateStructGEP(layouts.objects[owner.id], object, slot);
        auto* type = layouts.native(builder, rawType(id));
        return layouts.workers ? sharedRead(builder, type, field) : builder.CreateLoad(type, field);
    }

    Value emitNode(ExprId id, const Binding& binding) {
        auto value = emit(binding.value);
        if (live()) initialize(fn.expressions[id].target, value);
        return unit();
    }

    Value emitNode(ExprId id, const Block& block) {
        auto mark = id == bodyId() ? nullptr : cleanupMark();
        Value tail = unit();
        for (const auto& statement : block.statements) {
            tail = emit(statement.expression);
            if (!live()) break;
            if (statement.semicolon) tail = unit();
        }
        if (live() && mark) unwind(mark, handlers.size());
        return rawType(id) == Type::Unit ? unit() : tail;
    }

    Value emitNode(ExprId id, const Call& call) {
        const auto& info = fn.expressions[id];
        if (info.callee == Callee::Indirect) {
            Value closure;
            if (call.function) closure = emit(*call.function);
            else if (call.receiver) {
                auto object = emit(*call.receiver);
                if (!live()) return unit();
                auto owner = fn.expressions[*call.receiver].type;
                if (layouts.reference(owner)) {
                    auto address = builder.CreateStructGEP(layouts.objects[owner.id], object, info.target);
                    closure = layouts.workers ? sharedRead(builder, builder.getPtrTy(), address) : builder.CreateLoad(builder.getPtrTy(), address);
                } else closure = builder.CreateExtractValue(object, info.target);
            } else closure = read(info.target);
            if (!live()) return unit();
            std::vector<Value> args;
            for (const auto& arg : call.arguments) {
                args.push_back(emit(arg.value));
                if (!live()) return unit();
            }
            NativeClosures nativeCall{builder};
            const auto& shape = layouts.program.signatures[info.callableType->id];
            auto site = shape.result.kind == Type::Task ? nativeCall.location(source, call.name.span) : nullptr;
            auto value = nativeCall.invoke(signature(shape), closure, args, site, errorSlot);
            guard(builder.CreateLoad(builder.getPtrTy(), errorSlot), handlers.size(), call.name.span, "call");
            if (shape.result == Type::Never) { builder.CreateUnreachable(); end(); }
            return value;
        }
        auto skip = static_cast<std::size_t>(call.receiver && info.callee != Callee::Variant);
        Value receiver = nullptr;
        if (skip) {
            receiver = emit(*call.receiver);
            if (!live()) return unit();
        }
        auto* target = info.callee == Callee::Function ? &layouts.program.functions[info.target] : nullptr;
        std::vector<Value> args(target ? target->syntax.parameters.size() : call.arguments.size() + skip);
        if (receiver) args[0] = receiver;
        bool variadic = target && !target->syntax.parameters.empty() && target->syntax.parameters.back().variadic;
        NativeLists lists{builder};
        if (variadic) args.back() = lists.create();
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto slot = info.argumentSlots[index] + skip;
            auto value = emit(call.arguments[index].value);
            if (!live()) return unit();
            if (variadic && slot == args.size() - 1) {
                if (call.arguments[index].spread) lists.extend(args[slot], value);
                else lists.add(args[slot], value);
            } else args[slot] = value;
        }
        for (auto slot : info.omittedDefaults) {
            args[slot] = callFunction(*target->defaultFunctions[slot], llvm::ArrayRef<Value>(args).take_front(slot), call.name.span);
        }
        switch (info.callee) {
            case Callee::Indirect: llvm_unreachable("indirect call already emitted");
            case Callee::Exception: return NativeExceptions{builder}.create(args.front());
            case Callee::TaskCancel:
                builder.CreateCall(module.getOrInsertFunction("lyraTaskRequestCancel", builder.getVoidTy(), builder.getPtrTy()), args);
                return unit();
            case Callee::ListAdd: lists.add(args[0], args[1]); return unit();
            case Callee::Sum: return makeSum(rawType(id), static_cast<SumTag>(info.target), args[0]);
            case Callee::Variant: return makeVariant(id, args);
            case Callee::ToReal: return castNumber(receiver, fn.expressions[*call.receiver].type, rawType(id));
            case Callee::ToIntegerChecked: return checkedInteger(receiver, fn.expressions[*call.receiver].type, rawType(id));
            case Callee::ToString: return displayScalar(receiver, fn.expressions[*call.receiver].type);
            case Callee::ToBytes: return receiver;
            case Callee::DecodeUtf8: {
                auto pointer = builder.getPtrTy();
                auto check = module.getOrInsertFunction("lyraBytesUtf8", builder.getInt8Ty(), pointer);
                auto valid = builder.CreateIsNotNull(builder.CreateCall(check, {receiver}));
                auto option = layouts.native(builder, rawType(id));
                auto payload = builder.CreateSelect(valid, receiver, llvm::ConstantPointerNull::get(pointer));
                return builder.CreateInsertValue(builder.CreateInsertValue(llvm::Constant::getNullValue(option), valid, 0), payload, 1);
            }
            case Callee::Print: {
                auto type = fn.expressions[call.arguments[0].value].type;
                const char* symbol;
                if (type == Type::String || type == Type::Char || type == Type::Bool) {
                    symbol = "lyraPrintString";
                    args[0] = displayScalar(args[0], type);
                }
                else if (type.kind == Type::Real) symbol = numberWidth(type) == 32 ? "lyraPrintFloat32" : "lyraPrintFloat64";
                else {
                    symbol = type.kind == Type::Int ? "lyraPrintInt" : "lyraPrintUInt";
                    args[0] = castNumber(args[0], type, Type{type.kind});
                }
                builder.CreateCall(module.getOrInsertFunction(symbol, builder.getVoidTy(), args[0]->getType()), args);
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
            case Callee::Function: return callFunction(info.target, args, call.name.span);
        }
        llvm_unreachable("invalid Lyra callee");
    }

    void trapIf(Value condition, Span site, llvm::StringRef message) {
        auto failed = label("arithmetic.error"), next = label("arithmetic.ok");
        builder.CreateCondBr(condition, failed, next);
        builder.SetInsertPoint(failed);
        auto pointer = builder.getPtrTy();
        auto integer = builder.getInt64Ty();
        auto error = module.getOrInsertFunction("lyraArithmeticError", builder.getVoidTy(), pointer, integer, integer, pointer);
        auto position = source.position(site.begin);
        builder.CreateCall(error, {builder.CreateGlobalStringPtr(pathText(source.path)), builder.getInt64(position.line),
                                   builder.getInt64(position.column), builder.CreateGlobalStringPtr(message)});
        builder.CreateUnreachable();
        builder.SetInsertPoint(next);
    }

    Value arithmetic(Op op, Value left, Value right, Type type, Span site) {
        if (type.kind == Type::Real) {
            switch (op) {
                case Op::Add: return builder.CreateFAdd(left, right);
                case Op::Sub: return builder.CreateFSub(left, right);
                case Op::Mul: return builder.CreateFMul(left, right);
                case Op::Div: return builder.CreateFDiv(left, right);
                case Op::Rem: return builder.CreateFRem(left, right);
                default: llvm_unreachable("invalid floating point operation");
            }
        }
        auto* width = llvm::cast<llvm::IntegerType>(left->getType());
        auto signedType = type.kind == Type::Int;
        auto name = numberName(type);
        if (op == Op::Div || op == Op::Rem) {
            trapIf(builder.CreateIsNull(right), site, name + " division by zero");
            if (!signedType) return op == Op::Div ? builder.CreateUDiv(left, right) : builder.CreateURem(left, right);
            auto minusOne = builder.CreateICmpEQ(right, llvm::Constant::getAllOnesValue(width));
            if (op == Op::Rem) return builder.CreateSRem(left, builder.CreateSelect(minusOne, llvm::ConstantInt::get(width, 1), right));
            auto minimum = llvm::ConstantInt::get(width, llvm::APInt::getSignedMinValue(numberWidth(type)));
            trapIf(builder.CreateAnd(minusOne, builder.CreateICmpEQ(left, minimum)), site, name + " overflow (/)");
            return builder.CreateSDiv(left, right);
        }
        auto intrinsic = op == Op::Add ? (signedType ? llvm::Intrinsic::sadd_with_overflow : llvm::Intrinsic::uadd_with_overflow)
                       : op == Op::Sub ? (signedType ? llvm::Intrinsic::ssub_with_overflow : llvm::Intrinsic::usub_with_overflow)
                                       : (signedType ? llvm::Intrinsic::smul_with_overflow : llvm::Intrinsic::umul_with_overflow);
        auto checked = llvm::Intrinsic::getDeclaration(&module, intrinsic, {width});
        auto pair = builder.CreateCall(checked, {left, right});
        trapIf(builder.CreateExtractValue(pair, 1), site, name + (op == Op::Add ? " overflow (+)" : op == Op::Sub ? " overflow (-)" : " overflow (*)"));
        return builder.CreateExtractValue(pair, 0);
    }

    Value emitNode(ExprId, const Unary& unary) {
        auto value = emit(unary.operand);
        if (!live()) return unit();
        if (unary.op == Op::Not) return builder.CreateNot(value);
        auto type = fn.expressions[unary.operand].type;
        if (type.kind == Type::Real) return builder.CreateFNeg(value);
        return arithmetic(Op::Sub, llvm::Constant::getNullValue(value->getType()), value, type, unary.site);
    }

    Value emitNode(ExprId, const Binary& binary) {
        auto op = binary.op;
        if (op == Op::Eq || op == Op::Ne) {
            bool emptyLeft = std::holds_alternative<EmptyOption>(fn.syntax.expressions[binary.left].node);
            bool emptyRight = std::holds_alternative<EmptyOption>(fn.syntax.expressions[binary.right].node);
            if (emptyLeft || emptyRight) {
                auto value = emit(emptyLeft ? binary.right : binary.left);
                if (!live()) return unit();
                auto tag = builder.CreateExtractValue(value, 0);
                return op == Op::Eq ? builder.CreateNot(tag) : tag;
            }
        }
        if (op <= Op::RemSet) {
            if (fn.expressions[binary.left].type == Type::Never) return emit(binary.left);
            auto place = address(binary.left);
            if (!live()) return unit();
            auto type = fn.expressions[binary.left].type;
            auto shared = !place.list && layouts.workers && (!std::holds_alternative<Identifier>(fn.syntax.expressions[binary.left].node) || fn.locals[fn.expressions[binary.left].target].captured);
            auto left = op == Op::Set ? nullptr : shared ? sharedRead(builder, layouts.native(builder, type), place.pointer) : builder.CreateLoad(layouts.native(builder, type), place.pointer);
            auto right = emit(binary.right);
            if (!live()) return unit();
            constexpr Op compound[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Rem};
            if (op != Op::Set) {
                right = type == Type::String ? concatenate(left, right) : arithmetic(compound[static_cast<int>(op) - 1], left, right, type, binary.site);
            }
            if (place.list) {
                NativeLists lists{builder};
                if (!place.path.empty()) {
                    auto latest = lists.get(place.list, place.index, source, place.site);
                    right = builder.CreateInsertValue(builder.CreateLoad(layouts.native(builder, place.element), latest), right, place.path);
                }
                lists.set(place.list, place.index, right);
            } else if (shared) sharedWrite(builder, right, place.pointer);
            else builder.CreateStore(right, place.pointer);
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
        auto type = fn.expressions[binary.left].type;
        if (type == Type::Exception) {
            auto same = NativeExceptions{builder}.same(left, right);
            return op == Op::Eq ? same : builder.CreateNot(same);
        }
        if (type == Type::String || type == Type::Bytes) {
            if (op == Op::Add) return concatenate(left, right);
            auto equal = stringEqual(left, right);
            return op == Op::Eq ? equal : builder.CreateNot(equal);
        }
        if (type.kind == Type::Real) {
            switch (op) {
                case Op::Eq: return builder.CreateFCmpOEQ(left, right);
                case Op::Ne: return builder.CreateFCmpUNE(left, right);
                case Op::Lt: return builder.CreateFCmpOLT(left, right);
                case Op::Le: return builder.CreateFCmpOLE(left, right);
                case Op::Gt: return builder.CreateFCmpOGT(left, right);
                case Op::Ge: return builder.CreateFCmpOGE(left, right);
                default: return arithmetic(op, left, right, type, binary.site);
            }
        }
        auto unsignedType = type.kind == Type::UInt;
        switch (op) {
            case Op::Eq: return builder.CreateICmpEQ(left, right);
            case Op::Ne: return builder.CreateICmpNE(left, right);
            case Op::Lt: return unsignedType ? builder.CreateICmpULT(left, right) : builder.CreateICmpSLT(left, right);
            case Op::Le: return unsignedType ? builder.CreateICmpULE(left, right) : builder.CreateICmpSLE(left, right);
            case Op::Gt: return unsignedType ? builder.CreateICmpUGT(left, right) : builder.CreateICmpSGT(left, right);
            case Op::Ge: return unsignedType ? builder.CreateICmpUGE(left, right) : builder.CreateICmpSGE(left, right);
            default: return arithmetic(op, left, right, type, binary.site);
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
        auto type = rawType(id);
        if (type == Type::Unit) return unit();
        auto value = builder.CreatePHI(layouts.native(builder, type), 2);
        if (trueEnd) value->addIncoming(trueValue, trueEnd);
        if (falseEnd) value->addIncoming(falseValue, falseEnd);
        return value;
    }

    void loopBody(ExprId body, Label next, Label done) {
        loops.push_back({next, done, cleanupMark(), handlers.size()});
        emit(body);
        loops.pop_back();
        if (live()) builder.CreateBr(next);
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
        loopBody(loop.body, test, done);
        builder.SetInsertPoint(done);
        return unit();
    }

    template<class Element, class Advance>
    Value countedLoop(const For& loop, std::size_t slot, Value first, Value last, bool inclusive, Element element, Advance advance) {
        auto entry = builder.GetInsertBlock();
        auto test = label("for.test"), body = label("for.body"), step = label("for.step"), done = label("for.end");
        builder.CreateBr(test);
        builder.SetInsertPoint(test);
        auto index = builder.CreatePHI(builder.getInt64Ty(), 2, "for.index");
        index->addIncoming(first, entry);
        builder.CreateCondBr(inclusive ? builder.CreateICmpSLE(index, last) : builder.CreateICmpSLT(index, last), body, done);
        builder.SetInsertPoint(body);
        auto current = element(index);
        initialize(slot, current);
        loopBody(loop.body, step, done);
        builder.SetInsertPoint(step);
        if (inclusive) {
            auto increment = label("for.increment");
            builder.CreateCondBr(builder.CreateICmpEQ(index, last), done, increment);
            builder.SetInsertPoint(increment);
        }
        index->addIncoming(advance(index, current), builder.GetInsertBlock());
        builder.CreateBr(test);
        builder.SetInsertPoint(done);
        return unit();
    }

    template<class Element>
    Value countedLoop(const For& loop, std::size_t slot, Value first, Value last, bool inclusive, Element element) {
        return countedLoop(loop, slot, first, last, inclusive, element, [&](Value index, Value) { return builder.CreateAdd(index, builder.getInt64(1)); });
    }

    Value emitNode(ExprId id, const For& loop) {
        auto input = emit(loop.input);
        if (!live()) return unit();
        auto slot = fn.expressions[id].target;
        if (loop.range) {
            auto last = emit(loop.range->end);
            if (!live()) return unit();
            return countedLoop(loop, slot, input, last, loop.range->inclusive, [](Value index) { return index; });
        }
        auto site = fn.syntax.expressions[loop.input].span;
        if (const auto& plan = fn.expressions[id].iterator) {
            auto iterator = callFunction(plan->open, {input}, site);
            auto test = label("for.next"), body = label("for.body"), done = label("for.end");
            builder.CreateBr(test);
            builder.SetInsertPoint(test);
            auto next = callFunction(plan->next, {iterator}, site);
            builder.CreateCondBr(builder.CreateExtractValue(next, 0), body, done);
            builder.SetInsertPoint(body);
            initialize(slot, builder.CreateExtractValue(next, 1));
            loopBody(loop.body, test, done);
            builder.SetInsertPoint(done);
            return unit();
        }
        auto owner = fn.expressions[loop.input].type;
        if (owner == Type::String || owner == Type::Bytes) {
            auto pointer = builder.getPtrTy(); auto integer = builder.getInt64Ty();
            auto size = builder.CreateCall(module.getOrInsertFunction("lyraStringLength", integer, pointer), {input});
            auto element = layouts.native(builder, fn.locals[slot].type);
            auto get = module.getOrInsertFunction(owner == Type::String ? "lyraStringAtByte" : "lyraBytesAtByte", element, pointer, integer);
            return countedLoop(loop, slot, builder.getInt64(0), size, false, [&](Value index) {
                return builder.CreateCall(get, {input, index});
            }, [&](Value index, Value value) {
                if (owner == Type::Bytes) return builder.CreateAdd(index, builder.getInt64(1));
                auto width = builder.CreateCall(module.getOrInsertFunction("lyraCharWidth", element, element), {value});
                return builder.CreateAdd(index, builder.CreateZExt(width, integer));
            });
        }
        NativeLists lists{builder};
        auto type = layouts.native(builder, fn.locals[slot].type);
        return countedLoop(loop, slot, builder.getInt64(0), lists.size(input), false, [&](Value index) {
            return builder.CreateLoad(type, lists.get(input, index, source, site));
        });
    }

    Value emitNode(ExprId, const Return& ret) {
        auto value = initializer() ? read(fn.syntax.parameters.size()) : ret.value ? emit(*ret.value) : unit();
        if (live()) leave(value);
        end();
        return unit();
    }

    Value emitNode(ExprId, const Raise& raise) {
        auto error = raise.value ? emit(*raise.value) : caught.back();
        if (live()) propagate(raise.value ? NativeExceptions{builder}.locate(error, source, raise.site) : error, handlers.size());
        return unit();
    }

    Value emitNode(ExprId id, const Attempt& attempt) {
        auto landing = label("catch"), done = label("try.end");
        handlers.push_back({landing, cleanupMark()});
        auto value = emit(attempt.body);
        handlers.pop_back();
        auto normal = builder.GetInsertBlock();
        if (normal) builder.CreateBr(done);
        builder.SetInsertPoint(landing);
        auto error = builder.CreateLoad(builder.getPtrTy(), errorSlot);
        builder.CreateStore(NativeExceptions{builder}.none(), errorSlot);
        if (attempt.binding) initialize(fn.expressions[id].target, error);
        caught.push_back(error);
        auto fallback = emit(attempt.handler);
        caught.pop_back();
        auto handled = builder.GetInsertBlock();
        if (handled) builder.CreateBr(done);
        if (!normal && !handled) { done->eraseFromParent(); end(); return unit(); }
        builder.SetInsertPoint(done);
        auto type = rawType(id);
        if (type == Type::Unit) return unit();
        auto result = builder.CreatePHI(layouts.native(builder, type), 2);
        if (normal) result->addIncoming(value, normal);
        if (handled) result->addIncoming(fallback, handled);
        return result;
    }

    Value emitNode(ExprId, Jump jump) {
        auto [test, done, mark, depth] = loops.back();
        exitHandlers(depth);
        unwind(mark, depth);
        builder.CreateBr(jump == Jump::Break ? done : test);
        end();
        return unit();
    }
};

}

std::unique_ptr<llvm::Module> lower(const CheckedModule& program, const std::vector<Source>& sources, llvm::LLVMContext& context) {
    auto module = std::make_unique<llvm::Module>("lyra", context);
    module->setTargetTriple(nativeTriple);
    Builder builder(context);
    Layouts layouts(program, builder);
    std::vector<llvm::Function*> functions;
    for (const auto& fn : program.functions) {
        std::vector<llvm::Type*> params;
        for (std::size_t index = 0; index < fn.syntax.parameters.size(); ++index) params.push_back(layouts.native(builder, fn.locals[index].type));
        if (!fn.syntax.isAsync && !fn.syntax.foreignSymbol) params.push_back(builder.getPtrTy());
        auto signature = llvm::FunctionType::get(fn.syntax.isAsync ? builder.getPtrTy() : layouts.native(builder, fn.result), params, false);
        functions.push_back(llvm::Function::Create(signature, llvm::Function::InternalLinkage, "lyra.fn." + fn.syntax.name.text, *module));
    }
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const auto& fn = program.functions[index];
        auto* factory = functions[index];
        if (fn.syntax.foreignSymbol) {
            emitForeign(fn, sources[fn.syntax.file], *factory);
            continue;
        }
        auto* body = fn.syntax.isAsync ? asyncBody(*factory, layouts.native(builder, fn.result)) : factory;
        Emitter emitter{fn, sources[fn.syntax.file], *module, body, functions, layouts};
        emitter.run();
        if (fn.syntax.isAsync) lowerAsync(*factory, *body, emitter.cleanupHead);
    }
    bool commandLine = windows || module->getFunction("lyraIoArgCount") || module->getFunction("lyraIoArgument");
    auto signature = commandLine ? llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty(), builder.getPtrTy()}, false)
                                 : llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto main = llvm::Function::Create(signature, llvm::Function::ExternalLinkage, nativeEntry, *module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
    if (commandLine) builder.CreateCall(module->getOrInsertFunction("lyraIoSetArgs", builder.getVoidTy(), builder.getInt32Ty(), builder.getPtrTy()), {main->getArg(0), main->getArg(1)});
    auto entry = program.entry.value();
    const auto& root = program.functions[entry];
    if (root.syntax.isAsync) {
        auto result = NativeExceptions{builder}.created(builder.CreateCall(functions[entry]), sources[root.syntax.file], root.syntax.name.span);
        builder.CreateRet(asyncEntry(builder, result, root.result.kind == Type::Result ? layouts.native(builder, root.result) : nullptr));
    } else {
        auto slot = NativeExceptions{builder}.slot();
        builder.CreateCall(functions[entry], {slot});
        auto error = builder.CreateLoad(builder.getPtrTy(), slot);
        auto failed = llvm::BasicBlock::Create(context, "failed", main), success = llvm::BasicBlock::Create(context, "success", main);
        builder.CreateCondBr(builder.CreateIsNotNull(error), failed, success);
        builder.SetInsertPoint(failed);
        NativeExceptions{builder}.report(error);
        builder.CreateRet(builder.getInt32(1));
        builder.SetInsertPoint(success);
        builder.CreateRet(builder.getInt32(0));
    }
    std::string reason;
    llvm::raw_string_ostream log(reason);
    if (llvm::verifyModule(*module, &log)) throw Diagnostic{"LYB0001", "编译器生成的 LLVM 模块无效：" + reason};
    return module;
}

}
