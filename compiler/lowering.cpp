module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MathExtras.h>
#include <lyra/value_hash.hpp>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
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
    mutable std::map<Type, llvm::GlobalVariable*> typeTokens{};
    mutable std::set<Type> initializedTypeTokens{};
    mutable std::map<std::vector<std::size_t>, llvm::Constant*> interfaceTables{};
    mutable std::map<std::pair<Type, ValueOperation>, llvm::Function*> valueHelpers{};
    mutable std::map<Type, llvm::Function*> channelPackers{};
    bool workers{};

    Layouts(const CheckedModule& program, Builder& builder) : program(program) {
        for (const auto& fn : program.functions) {
            workers |= std::ranges::any_of(fn.syntax.expressions, [](const Expr& expr) {
                const auto* block = std::get_if<AsyncBlock>(&expr.node);
                return std::holds_alternative<Blocking>(expr.node) || (block && block->kind == AsyncBlock::Parallel);
            }) || std::ranges::any_of(fn.expressions, [](const CheckedExpr& expr) { return expr.callee == Callee::CallbackRegister; });
        }
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
            if (item.abstract) continue;
            for (const auto& payload : item.payloads) {
                std::vector<llvm::Type*> fields;
                for (auto field : payload) fields.push_back(native(builder, field));
                cases.push_back(llvm::StructType::get(builder.getContext(), fields));
            }
        }
    }

    bool reference(Type type) const { return program.aggregates[type.id].syntax.reference; }

    std::string typeName(Type type) const {
        auto nested = [&](Type item) { return typeName(item); };
        switch (type.kind) {
            case Type::Unit: return "Unit";
            case Type::Int: case Type::UInt: case Type::Real: return numberName(type);
            case Type::Bool: return "Bool";
            case Type::String: return "String";
            case Type::Char: return "Char";
            case Type::Bytes: return "Bytes";
            case Type::Never: return "Never";
            case Type::CPtr: return "CPtr";
            case Type::CCallback: return "CCallback";
            case Type::Any: return "Any";
            case Type::Error: return "Error";
            case Type::Exception:
                if (type == Type::Exception) return "Exception";
                if (type == channelClosed) return "ChannelClosed";
                return program.exceptions[type.id - firstDeclaredException].name.text;
            case Type::Record: case Type::Enum: {
                auto full = type.kind == Type::Record ? program.aggregates[type.id].syntax.name.text : program.enums[type.id].syntax.name.text;
                auto separator = full.rfind("::");
                std::string name = separator == full.npos ? full : full.substr(separator + 2);
                const auto& arguments = type.kind == Type::Record ? program.aggregates[type.id].arguments : program.enums[type.id].arguments;
                if (arguments.empty()) return name;
                name += '<';
                for (std::size_t index = 0; index < arguments.size(); ++index) {
                    if (index) name += ", ";
                    name += nested(arguments[index]);
                }
                return name + '>';
            }
            case Type::Task: return "Task<" + nested(program.taskResults[type.id]) + ">";
            case Type::TaskLocal: return "TaskLocal<" + nested(program.taskLocalValues[type.id]) + ">";
            case Type::Channel: return "Channel<" + nested(program.channelElements[type.id]) + ">";
            case Type::List: return "List<" + nested(program.listElements[type.id]) + ">";
            case Type::IntRange: return "Range<Int>";
            case Type::Option: return "Option<" + nested(program.sumTypes[type.id].success) + ">";
            case Type::Result: return "Result<" + nested(program.sumTypes[type.id].success) + ", " + nested(program.sumTypes[type.id].failure) + ">";
            case Type::Dyn: return lyra::typeName(program, type);
            case Type::Closure: {
                const auto& shape = program.signatures[type.id];
                std::string name = "(";
                for (std::size_t index = 0; index < shape.parameters.size(); ++index) {
                    if (index) name += ", ";
                    name += nested(shape.parameters[index]);
                }
                return name + ") -> " + nested(shape.result);
            }
            case Type::Variable: case Type::Projection: llvm_unreachable("abstract type cannot be reflected at runtime");
        }
        llvm_unreachable("invalid type name");
    }

    llvm::Constant* cString(Builder& builder, llvm::StringRef value) const {
        auto* content = llvm::ConstantDataArray::getString(builder.getContext(), value, true);
        auto* global = new llvm::GlobalVariable(*builder.GetInsertBlock()->getModule(), content->getType(), true,
            llvm::GlobalValue::PrivateLinkage, content, "lyra.reflect.text");
        llvm::Constant* indices[] = {builder.getInt32(0), builder.getInt32(0)};
        return llvm::ConstantExpr::getInBoundsGetElementPtr(content->getType(), global,
            indices);
    }

    llvm::Function* fieldGetter(Builder& builder, Type owner, std::size_t slot, Type fieldType) const {
        auto& module = *builder.GetInsertBlock()->getModule();
        auto* pointer = builder.getPtrTy();
        auto* signature = llvm::FunctionType::get(builder.getVoidTy(), {pointer, pointer}, false);
        auto* getter = llvm::Function::Create(signature, llvm::Function::InternalLinkage,
            "lyra.reflect.get." + std::to_string(owner.id) + "." + std::to_string(slot), module);
        Builder emit(llvm::BasicBlock::Create(builder.getContext(), "entry", getter));
        llvm::Value* object = getter->getArg(0);
        if (reference(owner)) object = emit.CreateLoad(pointer, object);
        auto* address = emit.CreateStructGEP(objects[owner.id], object, static_cast<unsigned>(slot));
        auto* fieldNative = native(emit, fieldType);
        auto* value = workers ? sharedRead(emit, fieldNative, address) : emit.CreateLoad(fieldNative, address);
        auto* anyType = llvm::StructType::get(emit.getContext(), {pointer, pointer});
        llvm::Value* any;
        if (fieldType == Type::Any) {
            any = value;
        } else {
            auto allocate = module.getOrInsertFunction("lyraGcAlloc", pointer, emit.getInt64Ty());
            auto* boxed = emit.CreateCall(allocate, {llvm::ConstantExpr::getSizeOf(fieldNative)});
            emit.CreateStore(value, boxed);
            any = llvm::UndefValue::get(anyType);
            any = emit.CreateInsertValue(any, typeToken(emit, fieldType), 0);
            any = emit.CreateInsertValue(any, boxed, 1);
        }
        emit.CreateStore(any, getter->getArg(1));
        emit.CreateRetVoid();
        return getter;
    }

    llvm::Constant* typeToken(Builder& builder, Type type) const {
        auto& module = *builder.GetInsertBlock()->getModule();
        auto* pointer = builder.getPtrTy();
        auto* descriptor = llvm::StructType::get(builder.getContext(), {builder.getInt32Ty(), builder.getInt32Ty(), pointer, builder.getInt64Ty(), pointer});
        auto [entry, inserted] = typeTokens.try_emplace(type);
        if (inserted) entry->second = new llvm::GlobalVariable(module, descriptor, true,
            llvm::GlobalValue::PrivateLinkage, llvm::Constant::getNullValue(descriptor), "lyra.type.identity");
        if (!initializedTypeTokens.contains(type)) {
            initializedTypeTokens.insert(type);
            std::vector<llvm::Constant*> fields;
            if (type.kind == Type::Record && !program.aggregates[type.id].abstract) {
                const auto& aggregate = program.aggregates[type.id];
                auto* fieldDescriptor = llvm::StructType::get(builder.getContext(), {pointer, pointer, pointer});
                for (std::size_t slot = 0; slot < aggregate.syntax.fields.size(); ++slot) {
                    const auto& field = aggregate.syntax.fields[slot];
                    if (!field.isPublic) continue;
                    auto* getter = fieldGetter(builder, type, slot, aggregate.fields[slot]);
                    fields.push_back(llvm::ConstantStruct::get(fieldDescriptor, {
                        cString(builder, field.name.text), typeToken(builder, aggregate.fields[slot]), getter
                    }));
                }
            }
            llvm::Constant* fieldArray = llvm::ConstantPointerNull::get(pointer);
            if (!fields.empty()) {
                auto* fieldDescriptor = llvm::StructType::get(builder.getContext(), {pointer, pointer, pointer});
                auto* array = llvm::ArrayType::get(fieldDescriptor, fields.size());
                auto* global = new llvm::GlobalVariable(module, array, true, llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(array, fields), "lyra.reflect.fields");
                llvm::Constant* indices[] = {builder.getInt32(0), builder.getInt32(0)};
                fieldArray = llvm::ConstantExpr::getInBoundsGetElementPtr(array, global,
                    indices);
            }
            unsigned kind = type.kind == Type::Record ? (reference(type) ? 2u : 1u) : type.kind == Type::Enum ? 3u : 0u;
            auto initializer = llvm::ConstantStruct::get(descriptor, {
                builder.getInt32(1), builder.getInt32(kind), cString(builder, typeName(type)),
                builder.getInt64(fields.size()), fieldArray
            });
            entry->second->setInitializer(initializer);
        }
        return entry->second;
    }

    llvm::Constant* interfaceTable(Builder& builder, const std::vector<std::size_t>& methods,
            std::span<llvm::Function* const> functions) const {
        auto [entry, inserted] = interfaceTables.try_emplace(methods);
        if (!inserted) return entry->second;
        auto& module = *builder.GetInsertBlock()->getModule();
        std::vector<llvm::Constant*> addresses;
        for (auto method : methods) {
            auto* implementation = functions[method];
            auto shape = implementation->getFunctionType()->params();
            std::vector<llvm::Type*> parameters(shape.begin(), shape.end());
            parameters[0] = builder.getPtrTy();
            auto* signature = llvm::FunctionType::get(implementation->getReturnType(), parameters, false);
            auto* bridge = llvm::Function::Create(signature, llvm::Function::InternalLinkage, implementation->getName() + ".interface", module);
            Builder thunk(llvm::BasicBlock::Create(builder.getContext(), "dispatch", bridge));
            std::vector<Value> arguments{thunk.CreateLoad(shape[0], bridge->getArg(0))};
            for (std::size_t slot = 1; slot < parameters.size(); ++slot) arguments.push_back(bridge->getArg(slot));
            thunk.CreateRet(thunk.CreateCall(implementation, arguments));
            addresses.push_back(bridge);
        }
        auto* type = llvm::ArrayType::get(builder.getPtrTy(), addresses.size());
        entry->second = new llvm::GlobalVariable(module, type, true, llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(type, addresses), "lyra.interface.dispatch");
        return entry->second;
    }

    llvm::Type* native(Builder& builder, Type type) const {
        switch (type.kind) {
        case Type::Int: case Type::UInt: return builder.getIntNTy(numberWidth(type));
        case Type::Real: return numberWidth(type) == 32 ? builder.getFloatTy() : builder.getDoubleTy();
        case Type::Bool: return builder.getInt1Ty();
        case Type::Char: return builder.getInt32Ty();
        case Type::Unit: case Type::Never: return builder.getInt8Ty();
        case Type::String: case Type::Bytes: case Type::Task: case Type::CPtr: case Type::List: case Type::Exception: case Type::Closure: case Type::Error: case Type::TaskLocal: case Type::Channel: case Type::CCallback: return builder.getPtrTy();
        case Type::Variable: case Type::Projection: llvm_unreachable("abstract type reached LLVM lowering");
        case Type::Any: case Type::Dyn: return llvm::StructType::get(builder.getContext(), {builder.getPtrTy(), builder.getPtrTy()});
        case Type::IntRange: return llvm::StructType::get(builder.getContext(), {builder.getInt64Ty(), builder.getInt64Ty(), builder.getInt1Ty()});
        case Type::Record: return reference(type) ? static_cast<llvm::Type*>(builder.getPtrTy()) : objects[type.id];
        case Type::Enum: return llvm::StructType::get(builder.getContext(), {builder.getInt32Ty(), builder.getPtrTy()});
        case Type::Option: return llvm::StructType::get(builder.getContext(), {builder.getInt1Ty(), native(builder, program.sumTypes[type.id].success)});
        case Type::Result: return llvm::StructType::get(builder.getContext(), {builder.getInt1Ty(), native(builder, program.sumTypes[type.id].success), native(builder, program.sumTypes[type.id].failure), builder.getPtrTy()});
        }
        llvm_unreachable("invalid Lyra type");
    }

    llvm::Function* channelPacker(Builder& builder, Type option) const {
        auto [entry, inserted] = channelPackers.try_emplace(option);
        if (!inserted) return entry->second;
        auto* pointer = builder.getPtrTy();
        auto* signature = llvm::FunctionType::get(pointer, {pointer}, false);
        auto* pack = llvm::Function::Create(signature, llvm::Function::InternalLinkage, "lyra.channel.pack", builder.GetInsertBlock()->getModule());
        entry->second = pack;
        auto* start = llvm::BasicBlock::Create(builder.getContext(), "entry", pack);
        auto* present = llvm::BasicBlock::Create(builder.getContext(), "some", pack);
        auto* absent = llvm::BasicBlock::Create(builder.getContext(), "none", pack);
        Builder emit(start);
        auto* boxed = pack->getArg(0);
        emit.CreateCondBr(emit.CreateIsNotNull(boxed), present, absent);
        auto* type = native(emit, option);
        auto* empty = llvm::Constant::getNullValue(type);
        emit.SetInsertPoint(absent);
        emit.CreateRet(NativeLists{emit}.box(empty));
        emit.SetInsertPoint(present);
        auto* payload = emit.CreateLoad(native(emit, program.sumTypes[option.id].success), boxed);
        auto* tagged = emit.CreateInsertValue(empty, emit.getTrue(), 0);
        emit.CreateRet(NativeLists{emit}.box(emit.CreateInsertValue(tagged, payload, 1)));
        return pack;
    }
};

struct NativeValues {
    const Layouts& layouts;
    const std::vector<llvm::Function*>& functions;

    struct Result { Value value; bool mayThrow; };

    static Value mix(Builder& builder, Value state, Value part) {
        auto mixed = builder.CreateMul(builder.CreateXor(state, part), builder.getInt64(valueHashFactor));
        return builder.CreateXor(mixed, builder.CreateLShr(mixed, 32));
    }

    Result emit(Builder& builder, Type type, ValueOperation operation, Value left, Value right, Value error) const {
        bool equality = operation == ValueOperation::Equal;
        std::vector<Value> args{left};
        if (equality) args.push_back(right);
        args.push_back(error);
        auto& module = *builder.GetInsertBlock()->getModule();
        if (auto found = layouts.program.valueOverrides.find({type, operation}); found != layouts.program.valueOverrides.end())
            return {builder.CreateCall(functions[found->second], args), true};
        if ((type.kind == Type::Record && !layouts.reference(type)) || type.kind == Type::Enum || type.kind == Type::Option || type.kind == Type::Result || type == Type::IntRange)
            return {builder.CreateCall(helper(builder, module, type, operation), args), true};
        auto pointer = builder.getPtrTy();
        bool text = type == Type::String || type == Type::Bytes;
        if (equality) {
            if (type == Type::Unit) return {builder.getTrue(), false};
            if (type.kind == Type::Real) return {builder.CreateFCmpOEQ(left, right), false};
            if (type.kind == Type::Exception) return {NativeExceptions{builder}.same(left, right), false};
            if (text) {
                auto same = module.getOrInsertFunction("lyraStringEqual", builder.getInt8Ty(), pointer, pointer);
                return {builder.CreateIsNotNull(builder.CreateCall(same, {left, right})), false};
            }
            return {builder.CreateICmpEQ(left, right), false};
        }
        if (text) return {builder.CreateCall(module.getOrInsertFunction("lyraStringHash", builder.getInt64Ty(), pointer), {left}), false};
        Value bits;
        if (type == Type::Unit) bits = builder.getInt64(0);
        else if (type.kind == Type::Real) {
            bits = numberWidth(type) == 32 ? builder.CreateZExt(builder.CreateBitCast(left, builder.getInt32Ty()), builder.getInt64Ty())
                                           : builder.CreateBitCast(left, builder.getInt64Ty());
            bits = builder.CreateSelect(builder.CreateFCmpOEQ(left, llvm::ConstantFP::get(left->getType(), 0.0)), builder.getInt64(0), bits);
        } else bits = builder.CreateZExtOrTrunc(left, builder.getInt64Ty());
        return {mix(builder, builder.getInt64(valueHashSeed), bits), false};
    }

    llvm::Function* helper(Builder& caller, llvm::Module& module, Type type, ValueOperation operation) const {
        auto [entry, inserted] = layouts.valueHelpers.try_emplace({type, operation});
        if (!inserted) return entry->second;
        bool equality = operation == ValueOperation::Equal;
        auto* native = layouts.native(caller, type);
        std::vector<llvm::Type*> params{native};
        if (equality) params.push_back(native);
        params.push_back(caller.getPtrTy());
        auto signature = llvm::FunctionType::get(equality ? caller.getInt1Ty() : caller.getInt64Ty(), params, false);
        auto* function = entry->second = llvm::Function::Create(signature, llvm::Function::InternalLinkage,
            equality ? "lyra.value.equal" : "lyra.value.hash", module);
        auto label = [&](const char* name) { return llvm::BasicBlock::Create(module.getContext(), name, function); };
        Builder builder(label("entry"));
        auto failure = label("failed");
        auto left = function->getArg(0);
        auto right = equality ? function->getArg(1) : nullptr;
        auto error = function->getArg(params.size() - 1);
        auto parts = [&](std::span<const Type> fields, Value hash, const auto& access) {
            for (unsigned slot = 0; slot < fields.size(); ++slot) {
                auto first = access(left, slot);
                auto second = equality ? access(right, slot) : nullptr;
                auto result = emit(builder, fields[slot], operation, first, second, error);
                if (result.mayThrow) {
                    auto success = label("field.returned");
                    builder.CreateCondBr(builder.CreateIsNull(builder.CreateLoad(builder.getPtrTy(), error)), success, failure);
                    builder.SetInsertPoint(success);
                }
                if (equality) {
                    auto next = label("field.equal");
                    builder.CreateCondBr(result.value, next, failure);
                    builder.SetInsertPoint(next);
                } else hash = mix(builder, hash, result.value);
            }
            builder.CreateRet(equality ? static_cast<Value>(builder.getTrue()) : hash);
        };
        auto* seed = builder.getInt64(valueHashSeed);
        if (type.kind == Type::Record || type == Type::IntRange) {
            auto fields = type == Type::IntRange ? std::span<const Type>(rangeFields) : std::span<const Type>(layouts.program.aggregates[type.id].fields);
            parts(fields, seed, [&](Value record, unsigned slot) {
                return builder.CreateExtractValue(record, slot);
            });
        } else {
            auto tag = builder.CreateExtractValue(left, 0);
            if (equality) {
                auto matched = label("tag.equal");
                builder.CreateCondBr(builder.CreateICmpEQ(tag, builder.CreateExtractValue(right, 0)), matched, failure);
                builder.SetInsertPoint(matched);
            }
            auto hash = equality ? nullptr : mix(builder, seed, builder.CreateZExt(tag, builder.getInt64Ty()));
            auto invalid = label("invalid.tag");
            auto count = type.kind == Type::Enum ? layouts.program.enums[type.id].payloads.size() : 2;
            auto branch = builder.CreateSwitch(tag, invalid, static_cast<unsigned>(count));
            for (unsigned index = 0; index < count; ++index) {
                auto active = label("active.payload");
                branch->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(tag->getType()), index), active);
                builder.SetInsertPoint(active);
                if (type.kind == Type::Enum) {
                    auto* storage = layouts.payloads[type.id][index];
                    parts(layouts.program.enums[type.id].payloads[index], hash, [&](Value item, unsigned slot) {
                        auto payload = builder.CreateExtractValue(item, 1);
                        return builder.CreateLoad(storage->getElementType(slot), builder.CreateStructGEP(storage, payload, slot));
                    });
                } else {
                    const auto& shape = layouts.program.sumTypes[type.id];
                    auto fields = index ? std::span{&shape.success, 1} : type.kind == Type::Result ? std::span{&shape.failure, 1} : std::span<const Type>{};
                    parts(fields, hash, [&](Value item, unsigned) { return builder.CreateExtractValue(item, index ? 1 : 2); });
                }
            }
            builder.SetInsertPoint(invalid);
            builder.CreateUnreachable();
        }
        builder.SetInsertPoint(failure);
        builder.CreateRet(llvm::Constant::getNullValue(function->getReturnType()));
        return function;
    }
};

struct DebugInfo {
    llvm::DIBuilder builder;
    llvm::DICompileUnit* unit;
    const llvm::DataLayout& abi;
    const Layouts& layouts;
    std::span<const Source> sources;
    Builder& ir;
    std::map<std::string, llvm::DIFile*> files{};
    std::map<Type, llvm::DIType*> types{};
    bool optimized;

    DebugInfo(llvm::Module& module, const Layouts& layouts, std::span<const Source> sources, Builder& ir, bool optimized)
        : builder(module), abi(module.getDataLayout()), layouts(layouts), sources(sources), ir(ir), optimized(optimized) {
        module.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        if constexpr (windows) module.addModuleFlag(llvm::Module::Warning, "CodeView", 1);
        else module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
        unit = builder.createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus_14, file(pathText(sources.front().path)),
            "Lyra compiler", optimized, "", 0);
    }

    llvm::DIFile* file(const std::string& path) {
        auto [entry, inserted] = files.try_emplace(path);
        if (inserted) {
            auto source = fromUtf8(path);
            entry->second = builder.createFile(pathText(source.filename()), pathText(source.parent_path()));
        }
        return entry->second;
    }

    llvm::DIType* pointer(llvm::DIType* target) {
        return builder.createPointerType(target, abi.getPointerSizeInBits(), abi.getPointerABIAlignment(0).value() * 8);
    }

    unsigned alignment(llvm::Type* native) const { return abi.getABITypeAlign(native).value() * 8; }

    llvm::DICompositeType* product(llvm::StringRef name, llvm::DIFile* source, unsigned line,
                                  llvm::StructType* storage, unsigned tag = llvm::dwarf::DW_TAG_structure_type) {
        auto forward = builder.createReplaceableCompositeType(tag, name, source, source, line, 0,
            abi.getTypeAllocSizeInBits(storage), alignment(storage), llvm::DINode::FlagZero);
        return llvm::MDNode::replaceWithDistinct(llvm::TempDICompositeType(forward));
    }

    llvm::DIDerivedType* member(llvm::DICompositeType* owner, llvm::StructType* storage, unsigned slot,
                                llvm::StringRef name, llvm::DIType* field, unsigned line,
                                llvm::DINode::DIFlags flags = llvm::DINode::FlagPublic) {
        auto* native = storage->getElementType(slot);
        return builder.createMemberType(owner, name, owner->getFile(), line, abi.getTypeStoreSizeInBits(native), alignment(native),
            abi.getStructLayout(storage)->getElementOffsetInBits(slot), flags, field);
    }

    llvm::DIType* record(Type value, llvm::DIType*& cached) {
        const auto& declaration = layouts.program.aggregates[value.id].syntax;
        const auto& source = sources[declaration.file];
        const auto site = source.userSite(declaration.name.span);
        auto* storage = layouts.objects[value.id];
        auto* compound = product(layouts.typeName(value), file(site.file), static_cast<unsigned>(site.start.line), storage,
            declaration.reference ? llvm::dwarf::DW_TAG_class_type : llvm::dwarf::DW_TAG_structure_type);
        cached = declaration.reference ? pointer(compound) : compound;
        std::vector<llvm::Metadata*> members;
        for (unsigned slot = 0; slot < declaration.fields.size(); ++slot) {
            const auto& field = declaration.fields[slot];
            auto line = static_cast<unsigned>(source.userSite(field.name.span).start.line);
            auto flags = field.isPublic ? llvm::DINode::FlagPublic : llvm::DINode::FlagPrivate;
            members.push_back(member(compound, storage, slot, field.name.text, type(layouts.program.aggregates[value.id].fields[slot]), line, flags));
        }
        builder.replaceArrays(compound, builder.getOrCreateArray(members));
        return cached;
    }

    llvm::DIType* sum(Type value, llvm::DIType*& cached) {
        auto* storage = llvm::cast<llvm::StructType>(layouts.native(ir, value));
        auto* compound = product(layouts.typeName(value), unit->getFile(), 0, storage);
        cached = compound;
        const auto& shape = layouts.program.sumTypes[value.id];
        const bool option = value.kind == Type::Option;
        std::vector<llvm::Metadata*> members{
            member(compound, storage, 0, option ? "hasValue" : "isOk", type(Type::Bool), 0),
            member(compound, storage, 1, "value", type(shape.success), 0),
        };
        if (!option) {
            members.push_back(member(compound, storage, 2, "error", type(shape.failure), 0));
            members.push_back(member(compound, storage, 3, "cause", type(Type::Error), 0));
        }
        builder.replaceArrays(compound, builder.getOrCreateArray(members));
        return cached;
    }

    llvm::DIType* range(Type value) {
        auto* storage = llvm::cast<llvm::StructType>(layouts.native(ir, value));
        auto* compound = product(layouts.typeName(value), unit->getFile(), 0, storage);
        std::vector<llvm::Metadata*> members;
        for (unsigned slot = 0; slot < std::size(rangeFields); ++slot)
            members.push_back(member(compound, storage, slot, rangeFieldNames[slot], type(rangeFields[slot]), 0));
        builder.replaceArrays(compound, builder.getOrCreateArray(members));
        return compound;
    }

    llvm::DIType* enumeration(Type value, llvm::DIType*& cached) {
        const auto& item = layouts.program.enums[value.id];
        const auto& source = sources[item.syntax.file];
        const auto site = source.userSite(item.syntax.name.span);
        auto line = static_cast<unsigned>(site.start.line);
        auto* location = file(site.file);
        auto* storage = llvm::cast<llvm::StructType>(layouts.native(ir, value));
        auto name = layouts.typeName(value);
        auto* compound = product(name, location, line, storage);
        cached = compound;
        std::vector<llvm::Metadata*> enumerators;
        std::vector<llvm::Metadata*> payloads;
        std::uint64_t size = 0;
        unsigned align = 0;
        for (unsigned index = 0; index < item.syntax.cases.size(); ++index) {
            const auto& variant = item.syntax.cases[index];
            enumerators.push_back(builder.createEnumerator(variant.name.text, index, true));
            if (variant.fields.empty()) continue;
            auto* payload = layouts.payloads[value.id][index];
            size = std::max(size, abi.getTypeAllocSizeInBits(payload).getFixedValue());
            align = std::max(align, alignment(payload));
            auto caseLine = static_cast<unsigned>(source.userSite(variant.name.span).start.line);
            auto* fields = product(name + "::" + variant.name.text, location, caseLine, payload);
            std::vector<llvm::Metadata*> members;
            for (unsigned slot = 0; slot < variant.fields.size(); ++slot) {
                const auto& field = variant.fields[slot];
                auto fieldName = field.name ? field.name->text : "item" + std::to_string(slot);
                members.push_back(member(fields, payload, slot, fieldName, type(item.payloads[index][slot]), caseLine));
            }
            builder.replaceArrays(fields, builder.getOrCreateArray(members));
            payloads.push_back(builder.createMemberType(location, variant.name.text, location, caseLine,
                abi.getTypeAllocSizeInBits(payload), alignment(payload), 0, llvm::DINode::FlagPublic, fields));
        }
        auto* tagType = builder.createEnumerationType(location, name + "::Tag", location, line, 32, 32,
            builder.getOrCreateArray(enumerators), type(Type{Type::UInt, 32}));
        auto* payloadType = payloads.empty() ? nullptr : builder.createUnionType(location, name + "::Payload", location, line,
            llvm::alignTo(size, align), align, llvm::DINode::FlagZero, builder.getOrCreateArray(payloads));
        llvm::Metadata* members[] = {member(compound, storage, 0, "tag", tagType, line),
            member(compound, storage, 1, "payload", pointer(payloadType), line)};
        builder.replaceArrays(compound, builder.getOrCreateArray(members));
        return cached;
    }

    llvm::DIType* type(Type value) {
        auto [entry, inserted] = types.try_emplace(value);
        auto& cached = entry->second;
        if (!inserted) return cached;
        switch (value.kind) {
            case Type::Record: return record(value, cached);
            case Type::Enum: return enumeration(value, cached);
            case Type::Option: case Type::Result: return sum(value, cached);
            case Type::IntRange: return cached = range(value);
            default: break;
        }
        auto name = layouts.typeName(value);
        auto basic = [&](unsigned bits, unsigned encoding) { return builder.createBasicType(name, bits, encoding); };
        switch (value.kind) {
            case Type::Int: cached = basic(numberWidth(value), llvm::dwarf::DW_ATE_signed); break;
            case Type::UInt: cached = basic(numberWidth(value), llvm::dwarf::DW_ATE_unsigned); break;
            case Type::Real: cached = basic(numberWidth(value), llvm::dwarf::DW_ATE_float); break;
            case Type::Bool: cached = basic(8, llvm::dwarf::DW_ATE_boolean); break;
            case Type::Char: cached = basic(32, llvm::dwarf::DW_ATE_unsigned_char); break;
            case Type::Unit: cached = basic(8, llvm::dwarf::DW_ATE_unsigned); break;
            case Type::CPtr: cached = pointer(nullptr); break;
            case Type::String: case Type::Bytes: case Type::Task: case Type::List: case Type::Exception:
            case Type::Closure: case Type::Error: case Type::TaskLocal: case Type::Channel: case Type::CCallback:
                cached = pointer(builder.createForwardDecl(llvm::dwarf::DW_TAG_structure_type, name, unit->getFile(), unit->getFile(), 0));
                break;
            default: cached = builder.createUnspecifiedType(name); break;
        }
        return cached;
    }

    llvm::DISubprogram* function(llvm::Function& native, const Site& site, llvm::ArrayRef<llvm::Metadata*> signature,
                                llvm::DINode::DIFlags flags = llvm::DINode::FlagZero) {
        auto* shape = builder.createSubroutineType(builder.getOrCreateTypeArray(signature));
        auto name = native.getName();
        auto line = static_cast<unsigned>(site.start.line);
        auto properties = llvm::DISubprogram::SPFlagDefinition;
        if (optimized) properties |= llvm::DISubprogram::SPFlagOptimized;
        auto* scope = builder.createFunction(unit, name, name, file(site.file), line, shape, line, flags, properties);
        native.setSubprogram(scope);
        return scope;
    }
};

struct Emitter {
    const CheckedFunction& fn;
    const Source& source;
    llvm::Module& module;
    llvm::Function* native;
    const std::vector<llvm::Function*>& functions;
    const Layouts& layouts;
    DebugInfo* debug;
    Builder builder{module.getContext()};
    llvm::DISubprogram* debugScope{};
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
    const Lambda* lambda() const { return worker ? std::get_if<Lambda>(&fn.syntax.expressions[*worker].node) : nullptr; }
    const AsyncBlock* asyncBlock() const { return worker ? std::get_if<AsyncBlock>(&fn.syntax.expressions[*worker].node) : nullptr; }
    ExprId bodyId() const {
        if (!worker) return fn.syntax.body;
        const auto& node = fn.syntax.expressions[*worker].node;
        if (auto cleanup = std::get_if<Deferred>(&node)) return cleanup->body;
        if (auto literal = std::get_if<Lambda>(&node)) return literal->body;
        if (auto block = asyncBlock()) return block->body;
        return std::get<Blocking>(node).body;
    }
    Type completionType() const {
        if (!worker) return fn.result;
        if (auto literal = lambda()) {
            auto result = layouts.program.signatures[rawType(*worker).id].result;
            return literal->isAsync ? layouts.program.taskResults[result.id] : result;
        }
        if (auto block = asyncBlock(); block && block->kind == AsyncBlock::Group) return rawType(*worker);
        return deferred() ? Type{Type::Unit} : layouts.program.taskResults[rawType(*worker).id];
    }
    Value cleanupMark() { return cleanupHead ? builder.CreateLoad(builder.getPtrTy(), cleanupHead) : nullptr; }
    bool asyncBody() const {
        if (auto literal = lambda()) return literal->isAsync;
        return worker ? asyncBlock() != nullptr : fn.syntax.isAsync;
    }

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

    Value valueOperation(Type type, ValueOperation operation, Value left, Value right, Span site) {
        auto result = NativeValues{layouts, functions}.emit(builder, type, operation, left, right, errorSlot);
        if (result.mayThrow) guard(builder.CreateLoad(builder.getPtrTy(), errorSlot), handlers.size(), site, "call");
        return result.value;
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
        if (debug) {
            const auto declaration = worker ? fn.syntax.expressions[*worker].span : fn.syntax.name.span;
            const auto site = source.userSite(declaration);
            std::vector<llvm::Metadata*> signature{debug->type(completionType())};
            if (!worker) {
                for (std::size_t slot = 0; slot < fn.syntax.parameters.size(); ++slot) signature.push_back(debug->type(fn.locals[slot].type));
            } else if (lambda()) {
                for (auto slot : fn.expressions[*worker].argumentSlots) signature.push_back(debug->type(fn.locals[slot].type));
            }
            debugScope = debug->function(*native, site, signature);
            debugLocation(site);
        }
        errorSlot = NativeExceptions{builder}.slot();
        if (std::ranges::find(fn.cleanupOwners, worker) != fn.cleanupOwners.end()) cleanupHead = cleanupStack(builder);
        locals.resize(fn.locals.size());
        for (std::size_t slot = 0; slot < fn.locals.size(); ++slot) {
            const auto& local = fn.locals[slot];
            if (local.worker == worker) {
                locals[slot] = builder.CreateAlloca(local.captured ? builder.getPtrTy() : layouts.native(builder, local.type));
                if (debug && local.declaration && !local.name.empty()) declareDebugLocal(slot, local);
            }
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

    llvm::DILocation* location(const Site& site) {
        auto* file = debug->file(site.file);
        llvm::DILocalScope* scope = debugScope;
        if (file != debugScope->getFile()) scope = debug->builder.createLexicalBlockFile(debugScope, file);
        return llvm::DILocation::get(module.getContext(), static_cast<unsigned>(site.start.line), static_cast<unsigned>(site.start.column), scope);
    }

    void debugLocation(const Site& site) { builder.SetCurrentDebugLocation(location(site)); }

    void declareDebugLocal(std::size_t slot, const Local& local) {
        const auto site = source.userSite(*local.declaration);
        auto* file = debug->file(site.file);
        auto* type = debug->type(local.type);
        auto line = static_cast<unsigned>(site.start.line);
        unsigned argument = 0;
        if (!worker && slot < fn.syntax.parameters.size()) argument = static_cast<unsigned>(slot + 1);
        if (lambda()) {
            const auto& slots = fn.expressions[*worker].argumentSlots;
            if (auto found = std::ranges::find(slots, slot); found != slots.end()) argument = static_cast<unsigned>(found - slots.begin() + 1);
        }
        auto* variable = argument ? debug->builder.createParameterVariable(debugScope, local.name, argument, file, line, type, true)
            : debug->builder.createAutoVariable(debugScope, local.name, file, line, type, true);
        auto* expression = local.captured ? debug->builder.createExpression({llvm::dwarf::DW_OP_deref}) : debug->builder.createExpression();
        debug->builder.insertDeclare(locals[slot], variable, expression, location(site), builder.GetInsertBlock());
    }

    bool initializer() const { return !worker && fn.syntax.initializer; }

    Type rawType(ExprId id) const { const auto& info = fn.expressions[id]; return info.sourceType.value_or(info.type); }

    Value makeSum(Type type, SumTag tag, Value payload = nullptr, Value previousError = nullptr) {
        Value value = llvm::Constant::getNullValue(layouts.native(builder, type));
        value = builder.CreateInsertValue(value, builder.getInt1(successTag(tag)), 0);
        if (previousError) value = builder.CreateInsertValue(value, previousError, 3);
        return payload ? builder.CreateInsertValue(value, payload, payloadSlot(tag)) : value;
    }

    Value makeVariant(Type type, std::size_t index, llvm::ArrayRef<Value> values) {
        auto tag = builder.getInt32(static_cast<unsigned>(index));
        Value result = builder.CreateInsertValue(llvm::Constant::getNullValue(layouts.native(builder, type)), tag, 0);
        if (values.empty()) return result;
        auto storage = layouts.payloads[type.id][index];
        auto payload = asyncCell(builder, storage);
        for (unsigned slot = 0; slot < values.size(); ++slot) builder.CreateStore(values[slot], builder.CreateStructGEP(storage, payload, slot));
        return builder.CreateInsertValue(result, payload, 1);
    }

    Value makeVariant(ExprId id, llvm::ArrayRef<Value> values = {}) {
        return makeVariant(rawType(id), fn.expressions[id].target, values);
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
        if (debug) debugLocation(source.userSite(fn.syntax.expressions[id].span));
        auto value = std::visit([&](const auto& node) { return emitNode(id, node); }, fn.syntax.expressions[id].node);
        const auto& info = fn.expressions[id];
        if (!live() || !info.sourceType) return value;
        if (info.conversion) return callFunction(*info.conversion, {value}, fn.syntax.expressions[id].span);
        if (info.type.kind == Type::Option) return makeSum(info.type, SumTag::Some, value);
        if (info.type == Type::Exception && info.sourceType->kind == Type::Exception) return value;
        if (info.type == Type::Any) return boxAny(value, *info.sourceType);
        if (info.type == Type::Error) return wrapError(value, *info.sourceType, info.errorPromotion, nullPointer(), fn.syntax.expressions[id].span);
        return castNumber(value, *info.sourceType, info.type);
    }

    Value nullPointer() { return llvm::ConstantPointerNull::get(builder.getPtrTy()); }

    Value boxAny(Value value, Type type) {
        if (type == Type::Any) return value;
        auto boxed = builder.CreateInsertValue(llvm::PoisonValue::get(layouts.native(builder, Type::Any)), layouts.typeToken(builder, type), 0);
        return builder.CreateInsertValue(boxed, NativeLists{builder}.box(value), 1);
    }

    Value pointerCall(llvm::StringRef name, llvm::ArrayRef<Value> arguments) {
        auto pointer = builder.getPtrTy();
        auto signature = llvm::FunctionType::get(pointer, std::vector<llvm::Type*>(arguments.size(), pointer), false);
        return builder.CreateCall(module.getOrInsertFunction(name, signature), arguments);
    }

    Value wrapError(Value value, Type type, const ErrorDisplay& display, Value cause, Span site) {
        if (type == Type::Error) return value;
        auto message = display.method ? callFunction(*display.method, {value}, site) : type == Type::String ? value : stringConstant(display.fallback);
        auto original = boxAny(value, type);
        return pointerCall("lyraErrorCreate", {builder.CreateExtractValue(original, 0), builder.CreateExtractValue(original, 1), message, cause});
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

    Value emitConstant(const ConstValue& constant, Type type, std::map<std::size_t, Value>& emitted) {
        auto* representation = layouts.native(builder, type);
        if (auto scalar = std::get_if<ConstScalar>(&constant)) {
            if (auto value = std::get_if<std::uint64_t>(scalar)) return llvm::ConstantInt::get(representation, *value);
            if (auto value = std::get_if<double>(scalar)) return llvm::ConstantFP::get(representation, *value);
            if (auto value = std::get_if<bool>(scalar)) return builder.getInt1(*value);
            if (auto value = std::get_if<std::string>(scalar)) return stringConstant(*value);
            return unit();
        }
        auto index = std::get<ConstRef>(constant).index;
        if (auto prior = emitted.find(index); prior != emitted.end()) return prior->second;
        const auto& stored = layouts.program.constantComposites[index];
        Value result;
        if (auto sum = std::get_if<SumTag>(&stored.tag)) {
            const auto& shape = layouts.program.sumTypes[type.id];
            auto payloadType = successTag(*sum) ? shape.success : shape.failure;
            result = makeSum(type, *sum, stored.fields.empty() ? nullptr : emitConstant(stored.fields.front(), payloadType, emitted));
        } else if (auto variant = std::get_if<VariantTag>(&stored.tag)) {
            std::vector<Value> fields;
            const auto& types = layouts.program.enums[type.id].payloads[variant->index];
            for (std::size_t slot = 0; slot < stored.fields.size(); ++slot) fields.push_back(emitConstant(stored.fields[slot], types[slot], emitted));
            result = makeVariant(type, variant->index, fields);
        } else {
            result = llvm::Constant::getNullValue(representation);
            auto types = type == Type::IntRange ? std::span<const Type>(rangeFields) : std::span<const Type>(layouts.program.aggregates[type.id].fields);
            for (unsigned slot = 0; slot < stored.fields.size(); ++slot)
                result = builder.CreateInsertValue(result, emitConstant(stored.fields[slot], types[slot], emitted), slot);
        }
        emitted.emplace(index, result);
        return result;
    }

    Value emitNode(ExprId id, const Identifier&) {
        const auto& info = fn.expressions[id];
        if (info.constantRef) {
            std::map<std::size_t, Value> emitted;
            return emitConstant(*layouts.program.constants[*info.constantRef], rawType(id), emitted);
        }
        if (info.functionRef) {
            const auto& syntax = layouts.program.functions[*info.functionRef].syntax;
            const auto& shape = layouts.program.signatures[rawType(id).id];
            return NativeClosures{builder}.named(functions[*info.functionRef], signature(shape), shape.parameters.size(), syntax.isAsync, syntax.foreignSymbol.has_value());
        }
        auto value = read(info.target);
        return info.narrowed ? builder.CreateExtractValue(value, 1) : value;
    }

    Value emitNode(ExprId id, This) { return read(fn.expressions[id].target); }

    Value emitNode(ExprId id, const Lambda& literal) {
        NativeClosures closures{builder};
        const auto& shape = layouts.program.signatures[rawType(id).id];
        auto* call = signature(shape);
        auto* code = closures.function(call, native->getName() + ".lambda");
        if (literal.isAsync) {
            auto* factory = closures.function(llvm::FunctionType::get(call->getReturnType(), call->params().drop_back(2), false), code->getName() + ".factory");
            auto* body = lyra::asyncBody(*factory, layouts.native(builder, layouts.program.taskResults[shape.result.id]));
            Emitter child{fn, source, module, body, functions, layouts, debug};
            child.run(id);
            lowerAsync(*factory, *body, child.cleanupHead);
            Builder invoke(llvm::BasicBlock::Create(builder.getContext(), "entry", code));
            std::vector<Value> arguments;
            for (std::size_t index = 0; index < factory->arg_size(); ++index) arguments.push_back(code->getArg(index));
            auto* task = invoke.CreateCall(factory, arguments);
            closures.noteTask(invoke, task, code->getArg(arguments.size()));
            invoke.CreateRet(task);
        } else Emitter{fn, source, module, code, functions, layouts, debug}.run(id);
        std::vector<Value> cells;
        for (auto slot : fn.expressions[id].captures) cells.push_back(cell(slot));
        return closures.close(code, cells);
    }

    Value emitNode(ExprId id, const TaskOp& op) {
        auto task = emit(op.operand);
        if (!live()) return unit();
        if (op.spawn) return asyncSpawn(builder, task);
        return awaitValue(id, task, op.site);
    }

    Value awaitValue(ExprId id, Value task, Span site) {
        return awaitValue(layouts.native(builder, rawType(id)), task, site);
    }

    Value awaitValue(llvm::Type* type, Value task, Span site) {
        auto result = asyncAwait(builder, type, task, errorSlot);
        guard(builder.CreateLoad(builder.getPtrTy(), errorSlot), handlers.size(), site, "await");
        return result;
    }

    Value emitNode(ExprId id, const Select& selection) {
        auto* pointer = builder.getPtrTy();
        auto* integer = builder.getInt64Ty();
        auto create = module.getOrInsertFunction("lyraTaskSelect", pointer, integer);
        auto add = module.getOrInsertFunction("lyraTaskSelectArm", builder.getVoidTy(), pointer, integer, pointer);
        auto task = builder.CreateCall(create, {builder.getInt64(selection.arms.size())});
        for (std::size_t index = 0; index < selection.arms.size(); ++index) {
            auto arm = emit(selection.arms[index].operation);
            if (!live()) return unit();
            builder.CreateCall(add, {task, builder.getInt64(index), arm});
        }
        auto outcome = awaitValue(llvm::StructType::get(builder.getContext(), {integer, pointer}), task, selection.site);
        auto invalid = label("select.invalid"), done = label("select.end");
        auto branch = builder.CreateSwitch(builder.CreateExtractValue(outcome, 0), invalid, selection.arms.size());
        std::vector<std::pair<Value, Label>> values;
        for (std::size_t index = 0; index < selection.arms.size(); ++index) {
            auto chosen = label("select.arm");
            branch->addCase(builder.getInt64(index), chosen);
            builder.SetInsertPoint(chosen);
            if (auto slot = fn.expressions[id].selectSlots[index]) {
                auto* type = layouts.native(builder, fn.locals[*slot].type);
                initialize(*slot, builder.CreateLoad(type, builder.CreateExtractValue(outcome, 1)));
            }
            auto value = emit(selection.arms[index].value);
            if (live()) { values.emplace_back(value, builder.GetInsertBlock()); builder.CreateBr(done); }
        }
        builder.SetInsertPoint(invalid);
        builder.CreateUnreachable();
        if (values.empty()) { done->eraseFromParent(); end(); return unit(); }
        builder.SetInsertPoint(done);
        auto result = rawType(id);
        if (result == Type::Unit) return unit();
        auto value = builder.CreatePHI(layouts.native(builder, result), values.size());
        for (auto [incoming, block] : values) value->addIncoming(incoming, block);
        return value;
    }

    std::pair<BlockingCode, std::vector<Value>> closure(ExprId id, Type result, llvm::StringRef suffix) {
        const auto& captures = fn.expressions[id].captures;
        auto code = blockingCode(*native, layouts.native(builder, result), captures.size(), suffix);
        Emitter{fn, source, module, code.body, functions, layouts, debug}.run(id);
        std::vector<Value> cells;
        for (auto slot : captures) cells.push_back(cell(slot));
        return {code, std::move(cells)};
    }

    Value emitNode(ExprId id, const Blocking&) {
        auto [code, cells] = closure(id, layouts.program.taskResults[rawType(id).id], ".worker");
        return NativeExceptions{builder}.created(blockingTask(builder, code, cells), source, fn.syntax.expressions[id].span);
    }

    Value emitNode(ExprId id, const AsyncBlock& block) {
        const auto& captures = fn.expressions[id].captures;
        auto parallel = block.kind == AsyncBlock::Parallel;
        auto group = block.kind == AsyncBlock::Group;
        auto type = rawType(id);
        if (!group) type = layouts.program.taskResults[type.id];
        auto* pointer = builder.getPtrTy();
        auto signature = llvm::FunctionType::get(pointer, std::vector<llvm::Type*>(captures.size(), pointer), false);
        auto* factory = llvm::Function::Create(signature, llvm::Function::InternalLinkage, native->getName() + (parallel ? ".parallel" : group ? ".group" : ".async"), module);
        auto* body = lyra::asyncBody(*factory, layouts.native(builder, type));
        Emitter child{fn, source, module, body, functions, layouts, debug};
        child.run(id);
        lowerAsync(*factory, *body, child.cleanupHead);
        std::vector<Value> cells;
        for (auto slot : captures) cells.push_back(cell(slot));
        auto task = NativeExceptions{builder}.created(builder.CreateCall(factory, cells), source, block.site);
        if (parallel) return asyncSpawn(builder, task, true);
        return group ? awaitValue(id, task, block.site) : task;
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
        Value result = nullptr, previous = nullptr;
        if (target.kind == Type::Result) {
            const auto& info = fn.expressions[id];
            auto from = layouts.program.sumTypes[fn.expressions[propagation.operand].type.id].failure;
            auto to = layouts.program.sumTypes[target.id].failure;
            auto failureValue = builder.CreateExtractValue(value, payloadSlot(SumTag::Err));
            previous = builder.CreateExtractValue(value, 3);
            result = failureValue;
            if (info.errorConversion) {
                result = callFunction(*info.errorConversion, {failureValue}, propagation.site);
                previous = wrapError(failureValue, from, info.errorDisplay, previous, propagation.site);
            } else if (from != to) {
                result = wrapError(failureValue, from, info.errorDisplay, previous, propagation.site);
                previous = nullptr;
            }
        }
        leave(makeSum(target, target.kind == Type::Option ? SumTag::None : SumTag::Err, result, previous));
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

    void emitPattern(const CheckedPattern& pattern, Value subject, Label miss, std::vector<std::pair<std::size_t, Value>>& bindings,
                     Span site, Value listBase = nullptr, Value listOffset = nullptr) {
        if (auto* slot = std::get_if<PatternSlot>(&pattern.node)) bindings.emplace_back(slot->local, subject);
        else if (auto* rest = std::get_if<PatternListRest>(&pattern.node)) {
            auto lists = NativeLists{builder};
            auto sourceList = listBase ? listBase : subject;
            auto start = listOffset ? listOffset : builder.getInt64(0);
            bindings.emplace_back(rest->local, lists.slice(sourceList, start));
        }
        else if (auto* constant = std::get_if<PatternConstant>(&pattern.node)) {
            std::map<std::size_t, Value> emitted;
            auto expected = emitConstant(*layouts.program.constants[constant->target], constant->type, emitted);
            matchTest(valueOperation(constant->type, ValueOperation::Equal, subject, expected, site), miss);
        }
        else if (auto* number = std::get_if<std::uint64_t>(&pattern.node))
            matchTest(builder.CreateICmpEQ(subject, llvm::ConstantInt::get(subject->getType(), *number)), miss);
        else if (auto* range = std::get_if<CheckedIntegerRange>(&pattern.node)) {
            auto lower = llvm::ConstantInt::get(subject->getType(), range->lower);
            auto upper = llvm::ConstantInt::get(subject->getType(), range->upper);
            auto aboveLower = range->signedValues ? builder.CreateICmpSGE(subject, lower) : builder.CreateICmpUGE(subject, lower);
            auto belowUpper = range->inclusive
                ? (range->signedValues ? builder.CreateICmpSLE(subject, upper) : builder.CreateICmpULE(subject, upper))
                : (range->signedValues ? builder.CreateICmpSLT(subject, upper) : builder.CreateICmpULT(subject, upper));
            matchTest(builder.CreateAnd(aboveLower, belowUpper), miss);
        } else if (std::holds_alternative<CheckedAlternatives>(pattern.node)) {
            auto matched = label("match.pattern.alternative");
            std::vector<std::pair<Label, std::vector<std::pair<std::size_t, Value>>>> incoming;
            for (std::size_t index = 0; index < pattern.parts.size(); ++index) {
                auto failed = index + 1 == pattern.parts.size() ? miss : label("match.pattern.next-alternative");
                std::vector<std::pair<std::size_t, Value>> alternativeBindings;
                emitPattern(pattern.parts[index], subject, failed, alternativeBindings, site, listBase, listOffset);
                auto predecessor = builder.GetInsertBlock();
                builder.CreateBr(matched);
                incoming.emplace_back(predecessor, std::move(alternativeBindings));
                if (index + 1 < pattern.parts.size()) builder.SetInsertPoint(failed);
            }
            builder.SetInsertPoint(matched);
            std::map<std::size_t, std::vector<std::pair<Value, Label>>> values;
            for (const auto& [block, alternativeBindings] : incoming)
                for (const auto& [slot, value] : alternativeBindings) values[slot].emplace_back(value, block);
            for (const auto& [slot, alternatives] : values) {
                if (alternatives.size() == 1) bindings.emplace_back(slot, alternatives.front().first);
                else {
                    auto phi = builder.CreatePHI(layouts.native(builder, fn.locals[slot].type), alternatives.size());
                    for (const auto& [value, block] : alternatives) phi->addIncoming(value, block);
                    bindings.emplace_back(slot, phi);
                }
            }
        }
        else if (auto* text = std::get_if<StringLiteral>(&pattern.node)) matchTest(stringEqual(subject, stringConstant(text->bytes)), miss);
        else if (auto* constructor = std::get_if<PatternConstructor>(&pattern.node)) {
            if (auto* tag = std::get_if<SumTag>(constructor)) {
                auto success = builder.CreateExtractValue(subject, 0);
                matchTest(successTag(*tag) ? success : builder.CreateNot(success), miss);
                if (*tag != SumTag::None) emitPattern(pattern.parts.front(), builder.CreateExtractValue(subject, payloadSlot(*tag)), miss, bindings, site);
            } else if (auto* variant = std::get_if<VariantTag>(constructor)) {
                matchTest(builder.CreateICmpEQ(builder.CreateExtractValue(subject, 0), builder.getInt32(variant->index)), miss);
                auto payload = builder.CreateExtractValue(subject, 1);
                auto storage = layouts.payloads[variant->owner][variant->index];
                for (unsigned slot = 0; slot < pattern.parts.size(); ++slot) {
                    const auto& part = pattern.parts[slot];
                    if (std::holds_alternative<std::monostate>(part.node)) continue;
                    auto value = builder.CreateLoad(storage->getElementType(slot), builder.CreateStructGEP(storage, payload, slot));
                    emitPattern(part, value, miss, bindings, site);
                }
            } else if (auto* record = std::get_if<PatternClass>(constructor)) {
                const auto& aggregate = layouts.program.aggregates[record->owner];
                for (unsigned slot = 0; slot < pattern.parts.size(); ++slot) {
                    const auto& part = pattern.parts[slot];
                    if (std::holds_alternative<std::monostate>(part.node)) continue;
                    auto* fieldType = layouts.native(builder, aggregate.fields[slot]);
                    auto* address = builder.CreateStructGEP(layouts.objects[record->owner], subject, slot);
                    auto value = layouts.workers ? sharedRead(builder, fieldType, address) : builder.CreateLoad(fieldType, address);
                    emitPattern(part, value, miss, bindings, site);
                }
            } else if (auto* list = std::get_if<ListTag>(constructor)) {
                auto lists = NativeLists{builder};
                auto snapshot = listBase ? listBase : lists.slice(subject, builder.getInt64(0));
                auto offset = listOffset ? listOffset : builder.getInt64(0);
                auto count = builder.CreateSub(lists.size(snapshot), offset);
                if (!list->cons) matchTest(builder.CreateICmpEQ(count, builder.getInt64(0)), miss);
                else {
                    matchTest(builder.CreateICmpSGT(count, builder.getInt64(0)), miss);
                    auto* elementType = layouts.native(builder, list->element);
                    auto head = builder.CreateLoad(elementType, lists.get(snapshot, offset, source, site));
                    emitPattern(pattern.parts[0], head, miss, bindings, site);
                    emitPattern(pattern.parts[1], snapshot, miss, bindings, site, snapshot, builder.CreateAdd(offset, builder.getInt64(1)));
                }
            } else if (auto* flag = std::get_if<bool>(constructor)) matchTest(*flag ? subject : builder.CreateNot(subject), miss);
            else for (unsigned slot = 0; slot < pattern.parts.size(); ++slot) {
                const auto& part = pattern.parts[slot];
                if (!std::holds_alternative<std::monostate>(part.node)) emitPattern(part, builder.CreateExtractValue(subject, slot), miss, bindings, site);
            }
        }
    }

    void emitBindings(const CheckedPattern& pattern, Value subject, Label miss, Span site) {
        std::vector<std::pair<std::size_t, Value>> bindings;
        emitPattern(pattern, subject, miss, bindings, site);
        for (auto [slot, payload] : bindings) initialize(slot, payload);
    }

    Value emitNode(ExprId id, const Match& match) {
        auto subject = emit(match.subject);
        if (!live()) return unit();
        auto done = label("match.end");
        std::vector<std::pair<Value, Label>> values;
        for (std::size_t index = 0; index < match.arms.size(); ++index) {
            const auto& arm = match.arms[index];
            auto next = label("match.next");
            emitBindings(fn.expressions[id].patterns[index], subject, next, arm.pattern.span);
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

    Value emitNode(ExprId id, const QualifiedMember&) { return makeVariant(id); }

    Value collectionSize(Type owner, Value value, bool byteCount = false) {
        if (owner.kind == Type::List) return NativeLists{builder}.size(value);
        auto symbol = owner == Type::String && !byteCount ? "lyraStringScalarCount" : "lyraStringLength";
        return builder.CreateCall(module.getOrInsertFunction(symbol, builder.getInt64Ty(), builder.getPtrTy()), {value});
    }

    Value emitNode(ExprId id, const Member& member) {
        if (fn.expressions[id].callee == Callee::Variant) return makeVariant(id);
        auto object = emit(member.receiver);
        if (!live()) return unit();
        if (fn.expressions[id].callee == Callee::ExtensionProperty)
            return callFunction(fn.expressions[id].target, {object}, member.name.span);
        auto owner = fn.expressions[member.receiver].type;
        if (owner.kind == Type::List || owner == Type::String || owner == Type::Bytes)
            return collectionSize(owner, object, member.name.text == "byteSize");
        if (owner == Type::IntRange) return builder.CreateExtractValue(object, static_cast<unsigned>(fn.expressions[id].target));
        if (owner == Type::Char) return object;
        if (owner.kind == Type::Exception) {
            switch (static_cast<ExceptionMember>(fn.expressions[id].target)) {
                case ExceptionMember::Message: return NativeExceptions{builder}.message(object);
                case ExceptionMember::TaskFailures: return pointerCall("lyraExceptionTaskFailures", {object});
                case ExceptionMember::CleanupFailures: return pointerCall("lyraExceptionCleanupFailures", {object});
            }
            llvm_unreachable("invalid Exception member");
        }
        if (owner.kind == Type::Error) {
            switch (static_cast<ErrorMember>(fn.expressions[id].target)) {
                case ErrorMember::Message: return pointerCall("lyraErrorMessage", {object});
                case ErrorMember::Original: {
                    auto value = builder.CreateInsertValue(llvm::PoisonValue::get(layouts.native(builder, Type::Any)), pointerCall("lyraErrorType", {object}), 0);
                    return builder.CreateInsertValue(value, pointerCall("lyraErrorValue", {object}), 1);
                }
                case ErrorMember::Cause: {
                    auto cause = pointerCall("lyraErrorCause", {object});
                    return builder.CreateInsertValue(makeSum(rawType(id), SumTag::Some, cause), builder.CreateIsNotNull(cause), 0);
                }
                case ErrorMember::Suppressed: return pointerCall("lyraErrorSuppressed", {object});
            }
            llvm_unreachable("invalid Error member");
        }
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

    Value emitNode(ExprId id, const PatternBinding& binding) {
        auto value = emit(binding.value);
        if (!live()) return unit();
        auto miss = label("binding.impossible");
        Builder{miss}.CreateUnreachable();
        emitBindings(fn.expressions[id].patterns.front(), value, miss, binding.pattern.span);
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

    Value emitNode(ExprId, const UnsafeBlock& block) { return emit(block.body); }

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
            } else {
                closure = read(info.target);
                if (info.narrowed) closure = builder.CreateExtractValue(closure, 1);
            }
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
        if (info.callee == Callee::TaskCancelled) return asyncTaskCancelled(builder);
        if (info.callee == Callee::TaskCheckCancellation) { asyncCheckCancellation(builder); return unit(); }
        bool namespaceCall = info.callee == Callee::Variant || info.callee == Callee::PtrNull ||
            info.callee == Callee::PtrAllocate || info.callee == Callee::CallbackRegister;
        auto skip = static_cast<std::size_t>(call.receiver && !namespaceCall);
        Value receiver = nullptr;
        if (skip) {
            receiver = emit(*call.receiver);
            if (!live()) return unit();
        }
        auto* target = info.callee == Callee::Function ? &layouts.program.functions[info.target] : nullptr;
        auto count = target ? target->syntax.parameters.size() : call.arguments.size() + skip;
        if (info.callee == Callee::Constructor) count = layouts.program.aggregates[info.target].fields.size();
        std::vector<Value> args(count);
        if (receiver) args[0] = receiver;
        bool variadic = target && !target->syntax.parameters.empty() && target->syntax.parameters.back().variadic;
        NativeLists lists{builder};
        if (variadic) args.back() = lists.create();
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto slot = info.argumentSlots[index] + skip;
            auto value = emit(call.arguments[index].value);
            if (!live()) return unit();
            if (variadic && slot == args.size() - 1) {
                if (fn.expressions[call.arguments[index].value].boxedSpread) {
                    auto type = layouts.program.listElements[fn.expressions[call.arguments[index].value].type.id];
                    lists.extendAny(args[slot], value, layouts.typeToken(builder, type));
                } else if (call.arguments[index].spread) lists.extend(args[slot], value);
                else lists.add(args[slot], value);
            } else args[slot] = value;
        }
        for (auto slot : info.omittedDefaults) {
            const auto& defaults = target ? target->defaultFunctions : layouts.program.aggregates[info.target].defaultFunctions;
            args[slot] = callFunction(*defaults[slot], llvm::ArrayRef<Value>(args).take_front(slot), call.name.span);
        }
        switch (info.callee) {
            case Callee::Indirect: llvm_unreachable("indirect call already emitted");
            case Callee::TaskCancelled: case Callee::TaskCheckCancellation: llvm_unreachable("static task call already emitted");
            case Callee::PtrAllocate: {
                auto* pointer = builder.getPtrTy();
                auto allocate = module.getOrInsertFunction("lyraPtrAllocate", pointer, builder.getInt64Ty(), pointer);
                builder.CreateStore(nullPointer(), errorSlot);
                auto* address = builder.CreateCall(allocate, {args[0], errorSlot});
                guard(builder.CreateLoad(pointer, errorSlot), handlers.size(), call.name.span, "call");
                return address;
            }
            case Callee::PtrNull: return nullPointer();
            case Callee::PtrIsNull: return builder.CreateIsNull(receiver);
            case Callee::PtrFree:
                builder.CreateCall(module.getOrInsertFunction("free", builder.getVoidTy(), builder.getPtrTy()), {receiver});
                return unit();
            case Callee::PtrOffset: case Callee::PtrRead: case Callee::PtrWrite: {
                auto* address = builder.CreateGEP(builder.getInt8Ty(), receiver, args[1]);
                if (info.callee == Callee::PtrOffset) return address;
                if (info.callee == Callee::PtrRead) return builder.CreateLoad(layouts.native(builder, rawType(id)), address);
                builder.CreateStore(args[2], address);
                return unit();
            }
            case Callee::CallbackRegister: {
                const auto& shape = layouts.program.signatures[info.callableType->id];
                auto* fallback = args.size() == 2 ? lists.box(args[1]) : nullPointer();
                return emitCallback(builder, *info.callableType, shape, signature(shape), args[0], fallback);
            }
            case Callee::CallbackContext: return pointerCall("lyraCallbackContext", {receiver});
            case Callee::CallbackFunction: return pointerCall("lyraCallbackFunction", {receiver});
            case Callee::CallbackClose:
                builder.CreateCall(module.getOrInsertFunction("lyraCallbackClose", builder.getVoidTy(), builder.getPtrTy()), {receiver});
                return unit();
            case Callee::CallbackTakeError: {
                auto* error = pointerCall("lyraCallbackTakeError", {receiver});
                return builder.CreateInsertValue(makeSum(rawType(id), SumTag::Some, error), builder.CreateIsNotNull(error), 0);
            }
            case Callee::ChannelCreate: {
                auto* pointer = builder.getPtrTy();
                auto create = module.getOrInsertFunction("lyraChannelCreate", pointer, builder.getInt64Ty(), pointer, pointer);
                builder.CreateStore(NativeExceptions{builder}.none(), errorSlot);
                auto channel = builder.CreateCall(create, {args[0], layouts.typeToken(builder, channelClosed), errorSlot});
                guard(builder.CreateLoad(pointer, errorSlot), handlers.size(), call.name.span, "call");
                return channel;
            }
            case Callee::ChannelSend:
                return NativeExceptions{builder}.created(pointerCall("lyraChannelSend", {receiver, lists.box(args[1])}), source, call.name.span);
            case Callee::ChannelReceive: {
                auto pack = layouts.channelPacker(builder, layouts.program.taskResults[rawType(id).id]);
                return NativeExceptions{builder}.created(pointerCall("lyraChannelReceive", {receiver, pack}), source, call.name.span);
            }
            case Callee::ChannelClose:
                builder.CreateCall(module.getOrInsertFunction("lyraChannelClose", builder.getVoidTy(), builder.getPtrTy()), args);
                return unit();
            case Callee::Sleep: {
                auto sleep = module.getOrInsertFunction("lyraTaskSleep", builder.getPtrTy(), builder.getInt64Ty());
                return NativeExceptions{builder}.created(builder.CreateCall(sleep, args), source, call.name.span);
            }
            case Callee::LocalKey: return asyncCell(builder, builder.getInt8Ty());
            case Callee::WithLocal: return NativeExceptions{builder}.created(pointerCall("lyraTaskWithLocal", {args[0], NativeLists{builder}.box(args[1]), args[2]}), source, call.name.span);
            case Callee::LocalGet: {
                auto context = pointerCall("lyraContextCurrent", {});
                auto box = pointerCall("lyraContextLocal", {context, receiver});
                auto option = rawType(id);
                auto payload = layouts.native(builder, layouts.program.sumTypes[option.id].success);
                auto missing = builder.GetInsertBlock(), found = label("local.found"), done = label("local.end");
                builder.CreateCondBr(builder.CreateIsNotNull(box), found, done);
                builder.SetInsertPoint(found);
                auto some = makeSum(option, SumTag::Some, builder.CreateLoad(payload, box));
                builder.CreateBr(done);
                builder.SetInsertPoint(done);
                auto* result = builder.CreatePHI(some->getType(), 2);
                result->addIncoming(llvm::Constant::getNullValue(some->getType()), missing);
                result->addIncoming(some, found);
                return result;
            }
            case Callee::BusinessError: {
                auto argument = call.arguments[std::ranges::find(info.argumentSlots, 0) - info.argumentSlots.begin()].value;
                auto cause = args.size() == 2 ? builder.CreateExtractValue(args[1], 1) : nullPointer();
                return wrapError(args[0], fn.expressions[argument].type, info.errorDisplay, cause, call.name.span);
            }
            case Callee::ErrorSuppress: return pointerCall("lyraErrorSuppress", args);
            case Callee::FailureInfo: {
                auto missing = builder.GetInsertBlock(), failure = label("result.error"), done = label("result.end");
                builder.CreateCondBr(builder.CreateExtractValue(receiver, 0), done, failure);
                builder.SetInsertPoint(failure);
                auto owner = fn.expressions[*call.receiver].type;
                auto error = wrapError(builder.CreateExtractValue(receiver, 2), layouts.program.sumTypes[owner.id].failure,
                    info.errorDisplay, builder.CreateExtractValue(receiver, 3), call.name.span);
                auto some = makeSum(rawType(id), SumTag::Some, error);
                failure = builder.GetInsertBlock();
                builder.CreateBr(done);
                builder.SetInsertPoint(done);
                auto* result = builder.CreatePHI(some->getType(), 2);
                result->addIncoming(llvm::Constant::getNullValue(some->getType()), missing);
                result->addIncoming(some, failure);
                return result;
            }
            case Callee::Dynamic: {
                auto* pointer = builder.getPtrTy();
                auto table = builder.CreateExtractValue(receiver, 1);
                auto method = builder.CreateLoad(pointer, builder.CreateInBoundsGEP(pointer, table, builder.getInt64(info.target)));
                args[0] = builder.CreateExtractValue(receiver, 0);
                if (!info.interfaceAsync) args.push_back(errorSlot);
                std::vector<llvm::Type*> parameters;
                for (auto value : args) parameters.push_back(value->getType());
                auto signature = llvm::FunctionType::get(layouts.native(builder, rawType(id)), parameters, false);
                auto value = builder.CreateCall(signature, method, args);
                if (info.interfaceAsync) return NativeExceptions{builder}.created(value, source, call.name.span);
                guard(builder.CreateLoad(pointer, errorSlot), handlers.size(), call.name.span, "call");
                return value;
            }
            case Callee::ReflectTypeName: return pointerCall("lyraReflectTypeName", {builder.CreateExtractValue(receiver, 0)});
            case Callee::ReflectFieldCount: {
                auto pointer = builder.getPtrTy();
                return builder.CreateCall(module.getOrInsertFunction("lyraReflectFieldCount", builder.getInt64Ty(), pointer),
                    {builder.CreateExtractValue(receiver, 0)});
            }
            case Callee::ReflectFieldName: return pointerCall("lyraReflectFieldNames", {builder.CreateExtractValue(receiver, 0)});
            case Callee::ReflectField: {
                auto pointer = builder.getPtrTy();
                auto anyType = layouts.native(builder, Type::Any);
                auto* input = builder.CreateAlloca(anyType);
                auto* output = builder.CreateAlloca(anyType);
                builder.CreateStore(receiver, input);
                builder.CreateStore(llvm::Constant::getNullValue(anyType), output);
                auto found = builder.CreateCall(module.getOrInsertFunction("lyraReflectFieldValue", builder.getInt8Ty(), pointer, pointer, pointer),
                    {input, args[1], output});
                auto missing = builder.GetInsertBlock(), present = label("reflect.field.present"), done = label("reflect.field.end");
                builder.CreateCondBr(builder.CreateICmpNE(found, builder.getInt8(0)), present, done);
                builder.SetInsertPoint(present);
                auto value = builder.CreateLoad(anyType, output);
                auto some = makeSum(rawType(id), SumTag::Some, value);
                auto presentEnd = builder.GetInsertBlock();
                builder.CreateBr(done);
                builder.SetInsertPoint(done);
                auto result = builder.CreatePHI(layouts.native(builder, rawType(id)), 2);
                result->addIncoming(makeSum(rawType(id), SumTag::None), missing);
                result->addIncoming(some, presentEnd);
                return result;
            }
            case Callee::Exception: {
                auto type = rawType(id);
                if (!type.id) return NativeExceptions{builder}.create(args.front());
                const auto name = layouts.typeName(type);
                auto separator = name.rfind("::");
                return NativeExceptions{builder}.create(args.front(), layouts.typeToken(builder, type),
                    std::string_view(name).substr(separator == name.npos ? 0 : separator + 2));
            }
            case Callee::TaskCancel:
                builder.CreateCall(module.getOrInsertFunction("lyraTaskRequestCancel", builder.getVoidTy(), builder.getPtrTy()), args);
                return unit();
            case Callee::ListAdd: lists.add(args[0], args[1]); return unit();
            case Callee::ListSlice: return lists.sliceRange(receiver, args[1], source, call.name.span);
            case Callee::Size: return collectionSize(fn.expressions[*call.receiver].type, receiver);
            case Callee::ValueEqual: case Callee::ValueHash:
                return valueOperation(fn.expressions[*call.receiver].type,
                    info.callee == Callee::ValueEqual ? ValueOperation::Equal : ValueOperation::Hash,
                    receiver, info.callee == Callee::ValueEqual ? args[1] : nullptr, call.name.span);
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
            case Callee::ExtensionProperty: llvm_unreachable("extension property getter is not a Call expression");
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

    Value emitNode(ExprId id, const TypeOperation& operation) {
        auto value = emit(operation.value);
        if (!live()) return unit();
        auto target = *fn.expressions[id].testedType;
        if (operation.op == TypeOperator::InterfaceCast) {
            auto from = fn.expressions[operation.value].type;
            if (from == target) return value;
            const auto& methods = fn.expressions[id].interfaceMethods;
            Value data, table;
            if (from.kind == Type::Dyn) {
                auto* pointer = builder.getPtrTy();
                auto original = builder.CreateExtractValue(value, 1);
                data = builder.CreateExtractValue(value, 0);
                table = asyncCell(builder, llvm::ArrayType::get(pointer, methods.size()));
                for (std::size_t slot = 0; slot < methods.size(); ++slot) {
                    auto method = builder.CreateLoad(pointer, builder.CreateInBoundsGEP(pointer, original, builder.getInt64(methods[slot])));
                    builder.CreateStore(method, builder.CreateInBoundsGEP(pointer, table, builder.getInt64(slot)));
                }
            } else {
                data = NativeLists{builder}.box(value);
                table = layouts.interfaceTable(builder, methods, functions);
            }
            auto record = builder.CreateInsertValue(llvm::PoisonValue::get(layouts.native(builder, target)), data, 0);
            return builder.CreateInsertValue(record, table, 1);
        }
        if (target == Type::Any) return operation.op == TypeOperator::Is ? static_cast<Value>(builder.getTrue()) : makeSum(rawType(id), SumTag::Some, value);
        auto matches = builder.CreateICmpEQ(builder.CreateExtractValue(value, 0), layouts.typeToken(builder, target));
        if (operation.op == TypeOperator::Is) return matches;
        auto converted = label("cast.ok"), merge = label("cast.end"), absent = builder.GetInsertBlock();
        builder.CreateCondBr(matches, converted, merge);
        builder.SetInsertPoint(converted);
        auto type = rawType(id);
        auto result = makeSum(type, SumTag::Some, builder.CreateLoad(layouts.native(builder, target), builder.CreateExtractValue(value, 1)));
        builder.CreateBr(merge);
        builder.SetInsertPoint(merge);
        auto* option = builder.CreatePHI(layouts.native(builder, type), 2);
        option->addIncoming(llvm::Constant::getNullValue(option->getType()), absent);
        option->addIncoming(result, converted);
        return option;
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
        if (op == Op::Eq || op == Op::Ne) {
            auto equal = valueOperation(type, ValueOperation::Equal, left, right, binary.site);
            return op == Op::Eq ? equal : builder.CreateNot(equal);
        }
        if (type == Type::String) return concatenate(left, right);
        if (type.kind == Type::Real) {
            switch (op) {
                case Op::Lt: return builder.CreateFCmpOLT(left, right);
                case Op::Le: return builder.CreateFCmpOLE(left, right);
                case Op::Gt: return builder.CreateFCmpOGT(left, right);
                case Op::Ge: return builder.CreateFCmpOGE(left, right);
                default: return arithmetic(op, left, right, type, binary.site);
            }
        }
        auto unsignedType = type.kind == Type::UInt;
        switch (op) {
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

    Value emitNode(ExprId id, const RangeValue& range) {
        auto start = emit(range.start);
        if (!live()) return unit();
        auto last = emit(range.end);
        if (!live()) return unit();
        auto result = llvm::PoisonValue::get(layouts.native(builder, rawType(id)));
        auto endpoints = builder.CreateInsertValue(builder.CreateInsertValue(result, start, 0), last, 1);
        return builder.CreateInsertValue(endpoints, builder.getInt1(range.inclusive), 2);
    }

    template<class Element, class Advance>
    Value countedLoop(const For& loop, std::size_t slot, Value first, Value last, Value inclusive, Element element, Advance advance) {
        auto entry = builder.GetInsertBlock();
        auto test = label("for.test"), body = label("for.body"), step = label("for.step"), done = label("for.end");
        builder.CreateBr(test);
        builder.SetInsertPoint(test);
        auto index = builder.CreatePHI(builder.getInt64Ty(), 2, "for.index");
        index->addIncoming(first, entry);
        auto within = builder.CreateICmpSLT(index, last);
        if (inclusive) within = builder.CreateOr(within, builder.CreateAnd(inclusive, builder.CreateICmpEQ(index, last)));
        builder.CreateCondBr(within, body, done);
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
    Value countedLoop(const For& loop, std::size_t slot, Value first, Value last, Value inclusive, Element element) {
        return countedLoop(loop, slot, first, last, inclusive, element, [&](Value index, Value) { return builder.CreateAdd(index, builder.getInt64(1)); });
    }

    Value emitNode(ExprId id, const For& loop) {
        auto input = emit(loop.input);
        if (!live()) return unit();
        auto slot = fn.expressions[id].target;
        auto owner = fn.expressions[loop.input].type;
        if (owner == Type::IntRange)
            return countedLoop(loop, slot, builder.CreateExtractValue(input, 0), builder.CreateExtractValue(input, 1),
                builder.CreateExtractValue(input, 2), [](Value index) { return index; });
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
        if (owner == Type::String || owner == Type::Bytes) {
            auto pointer = builder.getPtrTy(); auto integer = builder.getInt64Ty();
            auto size = builder.CreateCall(module.getOrInsertFunction("lyraStringLength", integer, pointer), {input});
            auto element = layouts.native(builder, fn.locals[slot].type);
            auto get = module.getOrInsertFunction(owner == Type::String ? "lyraStringAtByte" : "lyraBytesAtByte", element, pointer, integer);
            return countedLoop(loop, slot, builder.getInt64(0), size, nullptr, [&](Value index) {
                return builder.CreateCall(get, {input, index});
            }, [&](Value index, Value value) {
                if (owner == Type::Bytes) return builder.CreateAdd(index, builder.getInt64(1));
                auto width = builder.CreateCall(module.getOrInsertFunction("lyraCharWidth", element, element), {value});
                return builder.CreateAdd(index, builder.CreateZExt(width, integer));
            });
        }
        NativeLists lists{builder};
        auto type = layouts.native(builder, fn.locals[slot].type);
        return countedLoop(loop, slot, builder.getInt64(0), lists.size(input), nullptr, [&](Value index) {
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
        std::vector<std::pair<Value, Label>> values;
        auto finish = [&](Value result) {
            if (!live()) return;
            values.emplace_back(result, builder.GetInsertBlock());
            builder.CreateBr(done);
        };
        finish(value);
        builder.SetInsertPoint(landing);
        auto error = builder.CreateLoad(builder.getPtrTy(), errorSlot);
        NativeExceptions exceptions{builder};
        builder.CreateStore(exceptions.none(), errorSlot);
        const auto& checked = fn.expressions[id].catches;
        for (std::size_t slot = 0; slot < attempt.arms.size(); ++slot) {
            const auto& arm = attempt.arms[slot];
            const auto& plan = checked[slot];
            auto next = plan.type.id || arm.guard ? label("catch.next") : nullptr;
            if (plan.type.id) {
                auto typed = label("catch.typed");
                builder.CreateCondBr(exceptions.matches(error, layouts.typeToken(builder, plan.type)), typed, next);
                builder.SetInsertPoint(typed);
            }
            if (plan.local) initialize(*plan.local, error);
            caught.push_back(error);
            if (arm.guard) {
                auto accepted = emit(*arm.guard);
                if (live()) {
                    auto handler = label("catch.handle");
                    builder.CreateCondBr(accepted, handler, next);
                    builder.SetInsertPoint(handler);
                }
            }
            if (live()) finish(emit(arm.body));
            caught.pop_back();
            if (!next) { end(); break; }
            builder.SetInsertPoint(next);
        }
        if (live()) propagate(error, handlers.size());
        if (values.empty()) { done->eraseFromParent(); end(); return unit(); }
        builder.SetInsertPoint(done);
        auto type = rawType(id);
        if (type == Type::Unit) return unit();
        auto result = builder.CreatePHI(layouts.native(builder, type), values.size());
        for (const auto& [output, block] : values) result->addIncoming(output, block);
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

std::unique_ptr<llvm::Module> lower(const CheckedModule& program, const std::vector<Source>& sources,
        llvm::LLVMContext& context, bool optimized, bool symbols) {
    auto module = std::make_unique<llvm::Module>("lyra", context);
    module->setTargetTriple(nativeTriple);
    module->setDataLayout(nativeDataLayout);
    Builder builder(context);
    Layouts layouts(program, builder);
    std::optional<DebugInfo> debug;
    if (symbols) debug.emplace(*module, layouts, sources, builder, optimized);
    std::vector<llvm::Function*> functions;
    for (const auto& fn : program.functions) {
        if (fn.syntax.constant == ConstKind::Value) { functions.push_back(nullptr); continue; }
        std::vector<llvm::Type*> params;
        for (std::size_t index = 0; index < fn.syntax.parameters.size(); ++index) params.push_back(layouts.native(builder, fn.locals[index].type));
        if (!fn.syntax.isAsync && !fn.syntax.foreignSymbol) params.push_back(builder.getPtrTy());
        auto signature = llvm::FunctionType::get(fn.syntax.isAsync ? builder.getPtrTy() : layouts.native(builder, fn.result), params, false);
        functions.push_back(llvm::Function::Create(signature, llvm::Function::InternalLinkage, "lyra.fn." + fn.syntax.name.text, *module));
    }
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const auto& fn = program.functions[index];
        auto* factory = functions[index];
        if (!factory) continue;
        if (fn.syntax.foreignSymbol) {
            emitForeign(fn, sources[fn.syntax.file], *factory);
            continue;
        }
        auto* body = fn.syntax.isAsync ? asyncBody(*factory, layouts.native(builder, fn.result)) : factory;
        Emitter emitter{fn, sources[fn.syntax.file], *module, body, functions, layouts, debug ? &*debug : nullptr};
        emitter.run();
        if (fn.syntax.isAsync) lowerAsync(*factory, *body, emitter.cleanupHead);
    }
    bool commandLine = windows || module->getFunction("lyraIoArgCount") || module->getFunction("lyraIoArgument");
    auto signature = commandLine ? llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty(), builder.getPtrTy()}, false)
                                 : llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto main = llvm::Function::Create(signature, llvm::Function::ExternalLinkage, nativeEntry, *module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
    auto entry = program.entry.value();
    const auto& root = program.functions[entry];
    if (debug) {
        auto site = sources[root.syntax.file].userSite(root.syntax.name.span);
        llvm::Metadata* type[] = {debug->type(Type{Type::Int, 32})};
        auto* scope = debug->function(*main, site, type, llvm::DINode::FlagArtificial);
        builder.SetCurrentDebugLocation(llvm::DILocation::get(context, site.start.line, site.start.column, scope));
    }
    if (commandLine) builder.CreateCall(module->getOrInsertFunction("lyraIoSetArgs", builder.getVoidTy(), builder.getInt32Ty(), builder.getPtrTy()), {main->getArg(0), main->getArg(1)});
    if (root.syntax.isAsync) {
        auto result = NativeExceptions{builder}.created(builder.CreateCall(functions[entry]), sources[root.syntax.file], root.syntax.name.span);
        auto returnsResult = root.result.kind == Type::Result;
        builder.CreateRet(asyncEntry(builder, result, returnsResult ? layouts.native(builder, root.result) : nullptr,
            returnsResult && program.sumTypes[root.result.id].failure == Type::Error));
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
    if constexpr (!windows) for (auto& fn : *module) if (!fn.isDeclaration()) fn.setUWTableKind(llvm::UWTableKind::Async);
    if (debug) debug->builder.finalize();
    if (llvm::verifyModule(*module, &log)) throw Diagnostic{"LYB0001", "编译器生成的 LLVM 模块无效：" + reason};
    return module;
}

}
