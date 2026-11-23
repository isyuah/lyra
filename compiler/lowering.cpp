module;
#include <limits>
#include <algorithm>
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

namespace lyra {
namespace {

using Builder = llvm::IRBuilder<>;
using Value = llvm::Value*;
using Label = llvm::BasicBlock*;

struct Layouts {
    const CheckedModule& program;
    std::vector<llvm::StructType*> objects;
    bool workers{};

    Layouts(const CheckedModule& program, Builder& builder) : program(program) {
        for (const auto& fn : program.functions) workers |= std::ranges::any_of(fn.syntax.expressions, [](const Expr& expr) { return std::holds_alternative<Blocking>(expr.node); });
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
        case Type::String: case Type::Task: case Type::CPtr: case Type::List: return builder.getPtrTy();
        case Type::Variable: llvm_unreachable("abstract type reached LLVM lowering");
        case Type::Record: return reference(type) ? static_cast<llvm::Type*>(builder.getPtrTy()) : objects[type.id];
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
    struct Loop { Label test, done; Value cleanups; };
    std::vector<Loop> loops{};
    std::optional<ExprId> worker{};
    llvm::AllocaInst* cleanupHead{};

    Label label(const char* name) { return llvm::BasicBlock::Create(module.getContext(), name, native); }
    bool live() const { return builder.GetInsertBlock() != nullptr; }
    Value unit() { return builder.getInt8(0); }
    void end() { builder.ClearInsertionPoint(); }

    bool deferred() const { return worker && std::holds_alternative<Deferred>(fn.syntax.expressions[*worker].node); }
    ExprId bodyId() const {
        if (!worker) return fn.syntax.body;
        const auto& node = fn.syntax.expressions[*worker].node;
        if (auto cleanup = std::get_if<Deferred>(&node)) return cleanup->body;
        return std::get<Blocking>(node).body;
    }
    Type completionType() const {
        if (!worker) return fn.result;
        return deferred() ? Type{Type::Unit} : layouts.program.taskResults[rawType(*worker).id];
    }
    Value cleanupMark() { return cleanupHead ? builder.CreateLoad(builder.getPtrTy(), cleanupHead) : nullptr; }
    void unwind(Value mark = nullptr) { if (cleanupHead) cleanupUntil(builder, cleanupHead, mark); }
    void leave(Value value) {
        if (worker || !fn.syntax.isAsync) unwind();
        builder.CreateRet(value);
    }

    Value cell(std::size_t slot) {
        return fn.locals[slot].captured ? builder.CreateLoad(builder.getPtrTy(), locals[slot]) : locals[slot];
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
        if (std::ranges::find(fn.cleanupOwners, worker) != fn.cleanupOwners.end()) cleanupHead = cleanupStack(builder);
        locals.resize(fn.locals.size());
        for (std::size_t slot = 0; slot < fn.locals.size(); ++slot) {
            const auto& local = fn.locals[slot];
            if (local.worker == worker) locals[slot] = builder.CreateAlloca(local.captured ? builder.getPtrTy() : layouts.native(builder, local.type));
        }
        for (auto& arg : native->args()) {
            if (worker) {
                auto slot = fn.expressions[*worker].captures[arg.getArgNo()];
                locals[slot] = builder.CreateAlloca(builder.getPtrTy());
                builder.CreateStore(&arg, locals[slot]);
            } else initialize(arg.getArgNo(), &arg);
        }
        auto tail = emit(bodyId());
        if (live()) leave(deferred() ? unit() : tail);
    }

    Type rawType(ExprId id) const { const auto& info = fn.expressions[id]; return info.sourceType.value_or(info.type); }

    Value makeSum(Type type, SumTag tag, Value payload = nullptr) {
        Value value = llvm::Constant::getNullValue(layouts.native(builder, type));
        value = builder.CreateInsertValue(value, builder.getInt1(successTag(tag)), 0);
        return payload ? builder.CreateInsertValue(value, payload, payloadSlot(tag)) : value;
    }

    Value emit(ExprId id) {
        auto value = std::visit([&](const auto& node) { return emitNode(id, node); }, fn.syntax.expressions[id].node);
        const auto& info = fn.expressions[id];
        if (!live() || !info.sourceType) return value;
        return info.conversion ? builder.CreateCall(functions[*info.conversion], {value}) : makeSum(info.type, SumTag::Some, value);
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
        auto slot = fn.expressions[id].target;
        auto* type = layouts.native(builder, rawType(id));
        auto pointer = cell(slot);
        return fn.locals[slot].captured && layouts.workers ? sharedRead(builder, type, pointer) : builder.CreateLoad(type, pointer);
    }

    Value emitNode(ExprId id, const TaskOp& op) {
        auto task = emit(op.operand);
        if (!live()) return unit();
        return op.spawn ? asyncSpawn(builder, task) : asyncAwait(builder, layouts.native(builder, rawType(id)), task);
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
        return blockingTask(builder, code, cells);
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
        auto list = emit(index.receiver);
        if (!live()) return unit();
        auto offset = emit(index.offset);
        if (!live()) return unit();
        return builder.CreateLoad(layouts.native(builder, rawType(id)), NativeLists{builder}.get(list, offset, source, index.site));
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
            if (auto conversion = fn.expressions[id].errorConversion) result = builder.CreateCall(functions[*conversion], {result});
        }
        leave(makeSum(target, target.kind == Type::Option ? SumTag::None : SumTag::Err, result));
        builder.SetInsertPoint(success);
        return builder.CreateExtractValue(value, 1);
    }

    Value emitNode(ExprId id, const Match& match) {
        auto subject = emit(match.subject);
        if (!live()) return unit();
        auto done = label("match.end");
        std::vector<std::pair<Value, Label>> values;
        for (std::size_t index = 0; index < match.arms.size(); ++index) {
            const auto& arm = match.arms[index];
            auto armBody = label("match.arm"), next = label("match.next");
            Value payload = subject, selected = builder.getTrue();
            for (auto step : arm.pattern.tags) {
                auto success = builder.CreateExtractValue(payload, 0);
                selected = builder.CreateAnd(selected, successTag(step.tag) ? success : builder.CreateNot(success));
                if (step.tag != SumTag::None) payload = builder.CreateExtractValue(payload, payloadSlot(step.tag));
            }
            builder.CreateCondBr(selected, armBody, next);
            builder.SetInsertPoint(armBody);
            if (arm.pattern.binding) initialize(fn.expressions[id].argumentSlots[index], payload);
            auto value = emit(arm.value);
            if (live()) { values.emplace_back(value, builder.GetInsertBlock()); builder.CreateBr(done); }
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
        if (std::holds_alternative<Identifier>(node)) return {cell(fn.expressions[id].target)};
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
        auto object = emit(member.receiver);
        if (!live()) return unit();
        auto owner = fn.expressions[member.receiver].type;
        if (owner.kind == Type::List) return NativeLists{builder}.size(object);
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
        if (live() && mark) unwind(mark);
        return rawType(id) == Type::Unit ? unit() : tail;
    }

    Value emitNode(ExprId id, const Call& call) {
        const auto& info = fn.expressions[id];
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        Value receiver = nullptr;
        if (call.receiver) {
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
            auto helper = functions[*target->defaultFunctions[slot]];
            args[slot] = builder.CreateCall(helper, llvm::ArrayRef<Value>(args).take_front(slot));
        }
        switch (info.callee) {
            case Callee::TaskCancel:
                builder.CreateCall(module.getOrInsertFunction("lyraTaskRequestCancel", builder.getVoidTy(), builder.getPtrTy()), args);
                return unit();
            case Callee::ListAdd: lists.add(args[0], args[1]); return unit();
            case Callee::Sum: return makeSum(rawType(id), static_cast<SumTag>(info.target), args[0]);
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
                right = type == Type::String ? concatenate(left, right) : arithmetic(compound[static_cast<int>(op) - 1], left, right, binary.site);
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
        auto type = rawType(id);
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
        loops.push_back({test, done, cleanupMark()});
        emit(loop.body);
        loops.pop_back();
        if (live()) builder.CreateBr(test);
        builder.SetInsertPoint(done);
        return unit();
    }

    Value emitNode(ExprId, const Return& ret) {
        auto value = ret.value ? emit(*ret.value) : unit();
        if (live()) leave(value);
        end();
        return unit();
    }

    Value emitNode(ExprId, Jump jump) {
        auto [test, done, mark] = loops.back();
        unwind(mark);
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
    bool commandLine = module->getFunction("lyraIoArgCount") || module->getFunction("lyraIoArgument");
    auto signature = commandLine ? llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty(), builder.getPtrTy()}, false)
                                 : llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto main = llvm::Function::Create(signature, llvm::Function::ExternalLinkage, "main", *module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
    if (commandLine) builder.CreateCall(module->getOrInsertFunction("lyraIoSetArgs", builder.getVoidTy(), builder.getInt32Ty(), builder.getPtrTy()), {main->getArg(0), main->getArg(1)});
    auto entry = program.entry.value();
    auto result = builder.CreateCall(functions[entry]);
    const auto& root = program.functions[entry];
    if (root.syntax.isAsync) builder.CreateRet(asyncEntry(builder, result, root.result.kind == Type::Result ? layouts.native(builder, root.result) : nullptr));
    else builder.CreateRet(builder.getInt32(0));
    std::string reason;
    llvm::raw_string_ostream log(reason);
    if (llvm::verifyModule(*module, &log)) throw Diagnostic{"LYB0001", "编译器生成的 LLVM 模块无效：" + reason};
    return module;
}

}
