// Translator.cpp
// Copyright (c) 2014 - 2016, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.


#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include "llvm/IR/Value.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"

#include "llvm/Support/raw_ostream.h"

#include "ir/module.h"
#include "ir/constant.h"


#include "backend.h"

#include <set>


namespace Compiler
{
	static std::unordered_map<Identifier, llvm::StructType*> createdTypes;
	static std::map<fir::ConstantValue*, llvm::Constant*> cachedConstants;

	static std::map<fir::Type*, bool> searched;
	static bool isGenericInAnyWay(fir::Type* type)
	{
		if(searched.find(type) != searched.end())
			return searched[type];

		bool retval = false;
		if(type->isPointerType())			retval = isGenericInAnyWay(type->getPointerElementType());
		else if(type->isArrayType())		retval = isGenericInAnyWay(type->toArrayType()->getElementType());
		else if(type->isArraySliceType())	retval = isGenericInAnyWay(type->toArraySliceType()->getElementType());
		else if(type->isDynamicArrayType())	retval = isGenericInAnyWay(type->toDynamicArrayType()->getElementType());
		else if(type->isFunctionType())
		{
			bool ret = false;
			for(auto p : type->toFunctionType()->getArgumentTypes())
				ret = ret || isGenericInAnyWay(p);

			retval = ret || isGenericInAnyWay(type->toFunctionType()->getReturnType());
		}
		else if(type->isStructType())
		{
			// prevent recursive spiral of death
			searched[type] = false;

			bool ret = false;
			for(auto p : type->toStructType()->getElements())
				ret = ret || isGenericInAnyWay(p);

			retval = ret;
		}
		else if(type->isClassType())
		{
			// prevent recursive spiral of death
			searched[type] = false;

			bool ret = false;
			for(auto p : type->toClassType()->getElements())
				ret = ret || isGenericInAnyWay(p);

			// for(auto p : type->toClassType()->getMethods())
			// 	ret = ret || isGenericInAnyWay(p->getType());

			retval = ret;
		}
		else if(type->isTupleType())
		{
			bool ret = false;
			for(auto p : type->toTupleType()->getElements())
				ret = ret || isGenericInAnyWay(p);

			retval = ret;
		}
		else if(type->isEnumType())
		{
			retval = isGenericInAnyWay(type->toEnumType()->getCaseType());
		}
		else if(type->isParametricType())
		{
			retval = true;
		}
		else
		{
			retval = false;
		}

		return (searched[type] = retval);
	}


	static llvm::Type* typeToLlvm(fir::Type* type, llvm::Module* mod)
	{
		auto& gc = Compiler::LLVMBackend::getLLVMContext();
		if(type->isPrimitiveType())
		{
			fir::PrimitiveType* pt = type->toPrimitiveType();

			// signed/unsigned is lost.
			if(pt->isIntegerType())
			{
				return llvm::IntegerType::getIntNTy(gc, pt->getIntegerBitWidth());
			}
			else if(pt->isFloatingPointType())
			{
				if(pt->getFloatingPointBitWidth() == 32)
					return llvm::Type::getFloatTy(gc);

				else if(pt->getFloatingPointBitWidth() == 64)
					return llvm::Type::getDoubleTy(gc);

				else if(pt->getFloatingPointBitWidth() == 80)
					return llvm::Type::getX86_FP80Ty(gc);

				else if(pt->getFloatingPointBitWidth() == 128)
					return llvm::Type::getFP128Ty(gc);

				iceAssert(0);
			}
			else
			{
				iceAssert(0);
			}
		}
		else if(type->isStructType())
		{
			fir::StructType* st = type->toStructType();

			if(createdTypes.find(st->getStructName()) != createdTypes.end())
				return createdTypes[st->getStructName()];

			// to allow recursion, declare the type first.
			createdTypes[st->getStructName()] = llvm::StructType::create(gc, st->getStructName().mangled());

			std::vector<llvm::Type*> lmems;
			for(auto a : st->getElements())
				lmems.push_back(typeToLlvm(a, mod));

			createdTypes[st->getStructName()]->setBody(lmems, st->isPackedStruct());
			return createdTypes[st->getStructName()];
		}
		else if(type->isClassType())
		{
			fir::ClassType* ct = type->toClassType();

			if(createdTypes.find(ct->getClassName()) != createdTypes.end())
				return createdTypes[ct->getClassName()];

			// to allow recursion, declare the type first.
			createdTypes[ct->getClassName()] = llvm::StructType::create(gc, ct->getClassName().mangled());

			std::vector<llvm::Type*> lmems;
			for(auto a : ct->getElements())
				lmems.push_back(typeToLlvm(a, mod));

			createdTypes[ct->getClassName()]->setBody(lmems);
			return createdTypes[ct->getClassName()];
		}
		else if(type->isTupleType())
		{
			fir::TupleType* tt = type->toTupleType();

			std::vector<llvm::Type*> lmems;
			for(auto a : tt->getElements())
				lmems.push_back(typeToLlvm(a, mod));

			return llvm::StructType::get(gc, lmems);
		}
		else if(type->isFunctionType())
		{
			fir::FunctionType* ft = type->toFunctionType();
			std::vector<llvm::Type*> largs;
			for(auto a : ft->getArgumentTypes())
				largs.push_back(typeToLlvm(a, mod));

			// note(workaround): THIS IS A HACK.
			// we *ALWAYS* return a pointer to function, because llvm is stupid.
			// when we create an llvm::Function using this type, we always dereference the pointer type.
			// however, everywhere else (eg. function variables, parameters, etc.) we need pointers, because
			// llvm doesn't let FunctionType be a raw type (of a variable or param), but i'll let fir be less stupid,
			// so it transparently works without fir having to need pointers.
			return llvm::FunctionType::get(typeToLlvm(ft->getReturnType(), mod), largs, ft->isCStyleVarArg())->getPointerTo();
		}
		else if(type->isArrayType())
		{
			fir::ArrayType* at = type->toArrayType();
			return llvm::ArrayType::get(typeToLlvm(at->getElementType(), mod), at->getArraySize());
		}
		else if(type->isPointerType())
		{
			if(type->isVoidPointer())
				return llvm::Type::getInt8PtrTy(gc);

			else
				return typeToLlvm(type->getPointerElementType(), mod)->getPointerTo();
		}
		else if(type->isVoidType())
		{
			return llvm::Type::getVoidTy(gc);
		}
		else if(type->isNullType())
		{
			return llvm::Type::getVoidTy(gc)->getPointerTo();
		}
		else if(type->isDynamicArrayType())
		{
			fir::DynamicArrayType* llat = type->toDynamicArrayType();
			std::vector<llvm::Type*> mems;
			mems.push_back(typeToLlvm(llat->getElementType()->getPointerTo(), mod));
			mems.push_back(llvm::IntegerType::getInt64Ty(gc));
			mems.push_back(llvm::IntegerType::getInt64Ty(gc));

			return llvm::StructType::get(gc, mems, false);
		}
		else if(type->isArraySliceType())
		{
			fir::ArraySliceType* slct = type->toArraySliceType();
			std::vector<llvm::Type*> mems;
			mems.push_back(typeToLlvm(slct->getElementType()->getPointerTo(), mod));
			mems.push_back(llvm::IntegerType::getInt64Ty(gc));

			return llvm::StructType::get(gc, mems, false);
		}
		else if(type->isStringType())
		{
			llvm::Type* i8ptrtype = llvm::Type::getInt8PtrTy(gc);
			llvm::Type* i64type = llvm::Type::getInt64Ty(gc);

			auto id = Identifier("__string", IdKind::Struct);
			if(createdTypes.find(id) != createdTypes.end())
				return createdTypes[id];

			auto str = llvm::StructType::create(gc, id.mangled());
			str->setBody({ i8ptrtype, i64type });

			return createdTypes[id] = str;
		}
		else if(type->isCharType())
		{
			return llvm::Type::getInt8Ty(gc);
		}
		else if(type->isRangeType())
		{
			llvm::Type* i64type = llvm::Type::getInt64Ty(gc);

			auto id = Identifier("__range", IdKind::Struct);
			if(createdTypes.find(id) != createdTypes.end())
				return createdTypes[id];

			auto str = llvm::StructType::create(gc, id.mangled());
			str->setBody({ i64type, i64type });

			return createdTypes[id] = str;
		}
		else if(type->isEnumType())
		{
			return typeToLlvm(type->toEnumType()->getCaseType(), mod);
		}
		else if(type->isAnyType())
		{
			llvm::Type* i64type = llvm::Type::getInt64Ty(gc);
			llvm::Type* arrtype = llvm::ArrayType::get(llvm::Type::getInt8Ty(gc), 24);

			auto id = Identifier("__any", IdKind::Struct);
			if(createdTypes.find(id) != createdTypes.end())
				return createdTypes[id];

			auto str = llvm::StructType::create(gc, id.mangled());

			// typeid, flag, data
			str->setBody({ i64type, i64type, arrtype });

			return createdTypes[id] = str;
		}
		else if(type->isParametricType())
		{
			error("Cannot convert parametric type %s into LLVM, something went wrong", type->str().c_str());
		}
		else
		{
			error("Unimplememented type '%s' for LLVM backend", type->str().c_str());
		}
	}





	static llvm::Constant* constToLlvm(fir::ConstantValue* c, llvm::Module* mod)
	{
		iceAssert(c);
		auto ret = cachedConstants[c];
		if(ret) return ret;

		if(fir::ConstantInt* ci = dynamic_cast<fir::ConstantInt*>(c))
		{
			llvm::Type* it = typeToLlvm(c->getType(), mod);
			if(ci->getType()->toPrimitiveType()->isSigned())
			{
				return cachedConstants[c] = llvm::ConstantInt::getSigned(it, ci->getSignedValue());
			}
			else
			{
				return cachedConstants[c] = llvm::ConstantInt::get(it, ci->getUnsignedValue());
			}
		}
		else if(fir::ConstantChar* cc = dynamic_cast<fir::ConstantChar*>(c))
		{
			llvm::Type* ct = typeToLlvm(c->getType(), mod);
			return cachedConstants[c] = llvm::ConstantInt::get(ct, cc->getValue());
		}
		else if(fir::ConstantFP* cf = dynamic_cast<fir::ConstantFP*>(c))
		{
			llvm::Type* it = typeToLlvm(c->getType(), mod);
			return cachedConstants[c] = llvm::ConstantFP::get(it, cf->getValue());
		}
		else if(fir::ConstantArray* ca = dynamic_cast<fir::ConstantArray*>(c))
		{
			auto p = prof::Profile(PROFGROUP_LLVM, "const array");

			std::vector<llvm::Constant*> vals;
			vals.reserve(ca->getValues().size());

			for(auto con : ca->getValues())
				vals.push_back(constToLlvm(con, mod));

			return cachedConstants[c] = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(typeToLlvm(ca->getType(), mod)), vals);
		}
		else if(fir::ConstantTuple* ct = dynamic_cast<fir::ConstantTuple*>(c))
		{
			auto p = prof::Profile(PROFGROUP_LLVM, "const tuple");

			std::vector<llvm::Constant*> vals;
			vals.reserve(ct->getValues().size());

			for(auto v : ct->getValues())
				vals.push_back(constToLlvm(v, mod));

			return cachedConstants[c] = llvm::ConstantStruct::getAnon(Compiler::LLVMBackend::getLLVMContext(), vals);
		}
		else if(fir::ConstantString* cs = dynamic_cast<fir::ConstantString*>(c))
		{
			auto p = prof::Profile(PROFGROUP_LLVM, "const string");

			// note(portability): only works on 2's complement systems
			// where 0xFFFFFFFFFFFFFFFF == -1

			size_t origLen = cs->getValue().length();

			std::string str = cs->getValue();
			str.insert(str.begin(), 0xFF);
			str.insert(str.begin(), 0xFF);
			str.insert(str.begin(), 0xFF);
			str.insert(str.begin(), 0xFF);
			str.insert(str.begin(), 0xFF);
			str.insert(str.begin(), 0xFF);
			str.insert(str.begin(), 0xFF);
			str.insert(str.begin(), 0xFF);

			llvm::Constant* cstr = llvm::ConstantDataArray::getString(Compiler::LLVMBackend::getLLVMContext(), str, true);
			llvm::GlobalVariable* gv = new llvm::GlobalVariable(*mod, cstr->getType(), true,
				llvm::GlobalValue::LinkageTypes::InternalLinkage, cstr, "_FV_STR_" + std::to_string(cs->id));

			auto zconst = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), 0);
			std::vector<llvm::Constant*> indices = { zconst, zconst };
			llvm::Constant* gepd = llvm::ConstantExpr::getGetElementPtr(gv->getType()->getPointerElementType(), gv, indices);


			auto eightconst = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), 8);
			gepd = llvm::ConstantExpr::getInBoundsGetElementPtr(gepd->getType()->getPointerElementType(), gepd, eightconst);

			auto len = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), origLen);

			iceAssert(gepd->getType() == llvm::Type::getInt8PtrTy(Compiler::LLVMBackend::getLLVMContext()));
			iceAssert(len->getType() == llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()));

			std::vector<llvm::Constant*> mems = { gepd, len };
			auto ret = llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(typeToLlvm(fir::StringType::get(), mod)), mems);

			cachedConstants[c] = ret;
			return ret;
		}
		else if(fir::ConstantDynamicArray* cda = dynamic_cast<fir::ConstantDynamicArray*>(c))
		{
			if(cda->getArray())
			{
				llvm::Constant* constArray = constToLlvm(cda->getArray(), mod);
				iceAssert(constArray);

				// don't make it immutable. this probably puts the global variable in the .data segment, instead of the
				// .rodata/.rdata segment.

				// this allows us to modify it, eg.
				// var foo = [ 1, 2, 3 ]
				// foo[0] = 4

				// of course, since capacity == -1, the moment we try to like append or something,
				// we get back new heap memory

				llvm::GlobalVariable* tmpglob = new llvm::GlobalVariable(*mod, constArray->getType(), false,
					llvm::GlobalValue::LinkageTypes::InternalLinkage, constArray, "_FV_ARR_" + std::to_string(cda->id));

				auto zconst = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), 0);
				std::vector<llvm::Constant*> indices = { zconst, zconst };
				llvm::Constant* gepd = llvm::ConstantExpr::getGetElementPtr(tmpglob->getType()->getPointerElementType(), tmpglob, indices);

				auto flen = fir::ConstantInt::getInt64(cda->getArray()->getType()->toArrayType()->getArraySize());
				auto fcap = fir::ConstantInt::getInt64(-1);
				std::vector<llvm::Constant*> mems = { gepd, constToLlvm(flen, mod), constToLlvm(fcap, mod) };

				auto ret = llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(typeToLlvm(cda->getType(), mod)), mems);
				return cachedConstants[c] = ret;
			}
			else
			{
				std::vector<llvm::Constant*> mems = { constToLlvm(cda->getData(), mod), constToLlvm(cda->getLength(), mod),
					constToLlvm(cda->getCapacity(), mod) };

				auto ret = llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(typeToLlvm(cda->getType(), mod)), mems);
				return cachedConstants[c] = ret;
			}
		}
		else if(dynamic_cast<fir::ConstantStruct*>(c))
		{
			_error_and_exit("notsup const struct");
		}
		else
		{
			return cachedConstants[c] = llvm::Constant::getNullValue(typeToLlvm(c->getType(), mod));
		}
	}


	inline std::string llvmToString(llvm::Type* t)
	{
		std::string str;
		llvm::raw_string_ostream rso(str);
		t->print(rso);

		return str;
	}

	inline std::string llvmToString(llvm::Value* t)
	{
		std::string str;
		llvm::raw_string_ostream rso(str);
		t->getType()->print(rso);

		return str;
	}






	llvm::Module* LLVMBackend::translateFIRtoLLVM(fir::Module* firmod)
	{
		llvm::Module* module = new llvm::Module(firmod->getModuleName(), Compiler::LLVMBackend::getLLVMContext());

		// module->setDataLayout("e-m:o-i64:64-f80:128-n8:16:32:64-S128");
		llvm::IRBuilder<> builder(Compiler::LLVMBackend::getLLVMContext());

		createdTypes.clear();
		cachedConstants.clear();

		std::unordered_map<size_t, llvm::Value*>& valueMap = *(new std::unordered_map<size_t, llvm::Value*>());

		auto getValue = [&valueMap, &module, firmod](fir::Value* fv) -> llvm::Value* {

			if(fir::GlobalVariable* gv = dynamic_cast<fir::GlobalVariable*>(fv))
			{
				llvm::Value* lgv = valueMap[gv->id];
				if(!lgv)
					error("failed to find var %zu in mod %s\n", gv->id, firmod->getModuleName().c_str());

				iceAssert(lgv);
				return lgv;
			}
			// we must do this because function now derives from constantvalue
			else if(dynamic_cast<fir::Function*>(fv))
			{
				llvm::Value* ret = valueMap[fv->id];
				if(!ret) error("!ret (id = %zu)", fv->id);
				return ret;
			}
			else if(fir::ConstantValue* cv = dynamic_cast<fir::ConstantValue*>(fv))
			{
				return constToLlvm(cv, module);
			}
			else
			{
				llvm::Value* ret = valueMap[fv->id];
				if(!ret) error("!ret (id = %zu)", fv->id);
				return ret;
			}
		};

		auto getOperand = [&getValue](fir::Instruction* inst, size_t op) -> llvm::Value* {

			iceAssert(inst->operands.size() > op);
			fir::Value* fv = inst->operands[op];

			return getValue(fv);
		};

		auto addValueToMap = [&valueMap](llvm::Value* v, fir::Value* fv) {

			iceAssert(v);

			if(valueMap.find(fv->id) != valueMap.end())
				error("already have value with id %zu", fv->id);

			valueMap[fv->id] = v;
			// printf("adding value %zu\n", fv->id);

			if(!v->getType()->isVoidTy())
				v->setName(fv->getName().mangled());
		};




		static size_t strn = 0;
		for(auto string : firmod->_getGlobalStrings())
		{
			std::string id = "_FV_STR" + std::to_string(strn);

			llvm::Constant* cstr = llvm::ConstantDataArray::getString(Compiler::LLVMBackend::getLLVMContext(), string.first, true);
			llvm::Constant* gv = new llvm::GlobalVariable(*module, cstr->getType(), true,
				llvm::GlobalValue::LinkageTypes::InternalLinkage, cstr, id);

			auto zconst = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), 0);

			std::vector<llvm::Constant*> ix { zconst, zconst };
			gv = llvm::ConstantExpr::getInBoundsGetElementPtr(gv->getType()->getPointerElementType(), gv, ix);


			valueMap[string.second->id] = gv;
			strn++;
		}

		for(auto global : firmod->_getGlobals())
		{
			llvm::Constant* initval = 0;
			if(global.second->getInitialValue() != 0)
				initval = constToLlvm(global.second->getInitialValue(), module);

			llvm::GlobalVariable* gv = new llvm::GlobalVariable(*module, typeToLlvm(global.second->getType()->getPointerElementType(),
				module), false, global.second->linkageType == fir::LinkageType::External ? llvm::GlobalValue::LinkageTypes::ExternalLinkage : llvm::GlobalValue::LinkageTypes::InternalLinkage, initval, global.first.mangled());

			valueMap[global.second->id] = gv;
		}

		for(auto type : firmod->_getNamedTypes())
		{
			// should just automatically create it.
			if(!isGenericInAnyWay(type.second))
				typeToLlvm(type.second, module);
		}

		for(auto intr : firmod->_getIntrinsicFunctions())
		{
			auto& gc = Compiler::LLVMBackend::getLLVMContext();
			llvm::Constant* fn = 0;

			if(intr.first.str() == "memcpy")
			{
				llvm::FunctionType* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(gc), { llvm::Type::getInt8PtrTy(gc),
					llvm::Type::getInt8PtrTy(gc), llvm::Type::getInt64Ty(gc), llvm::Type::getInt32Ty(gc), llvm::Type::getInt1Ty(gc) }, false);
				fn = module->getOrInsertFunction("llvm.memcpy.p0i8.p0i8.i64", ft);
			}
			else if(intr.first.str() == "memmove")
			{
				llvm::FunctionType* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(gc), { llvm::Type::getInt8PtrTy(gc),
					llvm::Type::getInt8PtrTy(gc), llvm::Type::getInt64Ty(gc), llvm::Type::getInt32Ty(gc), llvm::Type::getInt1Ty(gc) }, false);
				fn = module->getOrInsertFunction("llvm.memmove.p0i8.p0i8.i64", ft);
			}
			else if(intr.first.str() == "memset")
			{
				llvm::FunctionType* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(gc), { llvm::Type::getInt8PtrTy(gc),
					llvm::Type::getInt8Ty(gc), llvm::Type::getInt64Ty(gc), llvm::Type::getInt32Ty(gc), llvm::Type::getInt1Ty(gc) }, false);
				fn = module->getOrInsertFunction("llvm.memset.p0i8.i64", ft);
			}
			else if(intr.first.str() == "memcmp")
			{
				// in line with the rest, take 5 arguments, the last 2 being alignment and isvolatile.

				llvm::FunctionType* ft = llvm::FunctionType::get(llvm::Type::getInt32Ty(gc), { llvm::Type::getInt8PtrTy(gc),
					llvm::Type::getInt8PtrTy(gc), llvm::Type::getInt64Ty(gc), llvm::Type::getInt32Ty(gc), llvm::Type::getInt1Ty(gc) }, false);

				fn = llvm::Function::Create(ft, llvm::GlobalValue::LinkageTypes::InternalLinkage, "fir.intrinsic.memcmp", module);
				llvm::Function* func = llvm::cast<llvm::Function>(fn);
				iceAssert(func);

				// ok... now make the function, right here.
				{
					func->addFnAttr(llvm::Attribute::AttrKind::AlwaysInline);
					llvm::BasicBlock* entry = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "entry", func);

					builder.SetInsertPoint(entry);

					/*
						basically:

						int counter = 0;
						while(counter < size)
						{
							i8 c1 = s1[counter];
							i8 c2 = s2[counter];

							if(c1 != c2)
								return c1 - c2;

							counter++;
						}
					*/

					llvm::Value* res = builder.CreateAlloca(llvm::Type::getInt32Ty(gc));
					llvm::Value* ptr1 = 0;
					llvm::Value* ptr2 = 0;
					llvm::Value* cmplen = 0;
					{
						// llvm is stupid.
						auto it = func->arg_begin();
						ptr1 = it;
						it++;

						ptr2 = it;
						it++;

						cmplen = it;
					}

					auto zeroconst = llvm::ConstantInt::get(gc, llvm::APInt(64, 0, true));
					auto zeroconst8 = llvm::ConstantInt::get(gc, llvm::APInt(8, 0, true));

					llvm::Value* ctr = builder.CreateAlloca(llvm::Type::getInt64Ty(gc));
					builder.CreateStore(zeroconst, ctr);

					llvm::BasicBlock* loopcond = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "loopcond", func);
					llvm::BasicBlock* loopbody = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "loopbody", func);
					llvm::BasicBlock* merge = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "merge", func);


					// explicit branch to loopcond
					builder.CreateBr(loopcond);

					builder.SetInsertPoint(loopcond);
					{
						// bounds check
						llvm::Value* cond = builder.CreateICmpSLT(builder.CreateLoad(ctr), cmplen);
						builder.CreateCondBr(cond, loopbody, merge);
					}

					builder.SetInsertPoint(loopbody);
					{
						llvm::Value* ctrval = builder.CreateLoad(ctr);

						llvm::Value* ch1 = builder.CreateLoad(builder.CreateInBoundsGEP(ptr1, ctrval));
						llvm::Value* ch2 = builder.CreateLoad(builder.CreateInBoundsGEP(ptr2, ctrval));

						llvm::Value* diff = builder.CreateSub(ch1, ch2);
						builder.CreateStore(builder.CreateIntCast(diff, llvm::Type::getInt32Ty(gc), false), res);

						builder.CreateStore(builder.CreateAdd(ctrval, llvm::ConstantInt::get(gc, llvm::APInt(64, 1, true))), ctr);

						builder.CreateCondBr(builder.CreateICmpEQ(diff, zeroconst8), loopcond, merge);
					}

					builder.SetInsertPoint(merge);
					{
						builder.CreateRet(builder.CreateLoad(res));
					}
				}
			}
			else if(intr.first.name == "roundup_pow2")
			{
				llvm::FunctionType* ft = llvm::FunctionType::get(llvm::Type::getInt64Ty(gc), { llvm::Type::getInt64Ty(gc) }, false);
				fn = llvm::Function::Create(ft, llvm::GlobalValue::LinkageTypes::InternalLinkage, "fir.intrinsic.roundup_pow2", module);

				llvm::Function* func = llvm::cast<llvm::Function>(fn);
				iceAssert(func);


				// ok... now make the function, right here.
				{
					func->addFnAttr(llvm::Attribute::AttrKind::AlwaysInline);
					llvm::BasicBlock* entry = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "entry", func);

					builder.SetInsertPoint(entry);

					/*
						basically:

						int num = arg;
						int ret = 1;

						while(num > 0)
						{
							num >>= 1
							ret <<= 1;
						}

						return ret;
					*/

					llvm::Value* num = builder.CreateAlloca(llvm::Type::getInt64Ty(gc));
					llvm::Value* retval = builder.CreateAlloca(llvm::Type::getInt64Ty(gc));

					auto oneconst = llvm::ConstantInt::get(gc, llvm::APInt(64, 1, true));
					auto zeroconst = llvm::ConstantInt::get(gc, llvm::APInt(64, 0, true));

					builder.CreateStore(oneconst, retval);
					builder.CreateStore(func->arg_begin(), num);


					llvm::BasicBlock* loopcond = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "loopcond", func);
					llvm::BasicBlock* loopbody = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "loopbody", func);
					llvm::BasicBlock* merge = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), "merge", func);


					// explicit branch to loopcond
					builder.CreateBr(loopcond);

					builder.SetInsertPoint(loopcond);
					{
						// bounds check
						llvm::Value* cond = builder.CreateICmpSGT(builder.CreateLoad(num), zeroconst);
						builder.CreateCondBr(cond, loopbody, merge);
					}

					builder.SetInsertPoint(loopbody);
					{
						llvm::Value* shifted = builder.CreateLShr(builder.CreateLoad(num), 1);
						builder.CreateStore(shifted, num);

						builder.CreateStore(builder.CreateShl(builder.CreateLoad(retval), 1), retval);

						builder.CreateBr(loopcond);
					}

					builder.SetInsertPoint(merge);
					{
						builder.CreateRet(builder.CreateLoad(retval));
					}
				}
			}
			else
			{
				error("unknown intrinsic '%s'", intr.first.str().c_str());
			}

			valueMap[intr.second->id] = fn;
		}

		// fprintf(stderr, "translating module %s\n", this->moduleName.c_str());
		for(auto f : firmod->_getFunctions())
		{
			fir::Function* ffn = f.second;

			if(isGenericInAnyWay(ffn->getType()))
				continue;

			llvm::GlobalValue::LinkageTypes link;
			switch(ffn->linkageType)
			{
				case fir::LinkageType::External:
					link = llvm::GlobalValue::LinkageTypes::ExternalLinkage;
					break;

				case fir::LinkageType::Internal:
					link = llvm::GlobalValue::LinkageTypes::InternalLinkage;
					break;

				case fir::LinkageType::ExternalWeak:
					link = llvm::GlobalValue::LinkageTypes::ExternalWeakLinkage;
					break;

				default:
					error("enotsup");
			}


			llvm::FunctionType* ftype = llvm::cast<llvm::FunctionType>(typeToLlvm(ffn->getType(), module)->getPointerElementType());
			llvm::Function* func = llvm::Function::Create(ftype, link, ffn->getName().mangled(), module);

			if(ffn->isAlwaysInlined())
				func->addFnAttr(llvm::Attribute::AttrKind::AlwaysInline);

			valueMap[ffn->id] = func;

			size_t i = 0;
			for(auto it = func->arg_begin(); it != func->arg_end(); it++, i++)
			{
				valueMap[ffn->getArguments()[i]->id] = it;

				// fprintf(stderr, "adding func arg %zu\n", ffn->getArguments()[i]->id);
			}


			for(auto b : ffn->getBlockList())
			{
				llvm::BasicBlock* bb = llvm::BasicBlock::Create(Compiler::LLVMBackend::getLLVMContext(), b->getName().mangled(), func);
				valueMap[b->id] = bb;

				// fprintf(stderr, "adding block %zu\n", b->id);
			}
		}


		#define DO_DUMP 0

		#if DO_DUMP
		#define DUMP_INSTR(fmt, ...)		(fprintf(stderr, fmt, ##__VA_ARGS__))
		#else
		#define DUMP_INSTR(...)
		#endif


		for(auto fp : firmod->_getFunctions())
		{
			fir::Function* ffn = fp.second;

			if(isGenericInAnyWay(ffn->getType()))
				continue;

			llvm::Function* func = module->getFunction(fp.second->getName().mangled());
			iceAssert(func);


			DUMP_INSTR("%s%s", ffn->getBlockList().size() == 0 ? "declare " : "", ffn->getName().c_str());

			if(ffn->getBlockList().size() > 0)
			{
				// print args
				DUMP_INSTR(" :: (");

				size_t i = 0;
				for(auto arg : ffn->getArguments())
				{
					DUMP_INSTR("%%%zu :: %s", arg->id, arg->getType()->str().c_str());
					i++;

					(void) arg;

					if(i != ffn->getArgumentCount())
						DUMP_INSTR(",");
				}

				DUMP_INSTR(")");
			}
			else
			{
				DUMP_INSTR(" :: %s\n", ffn->getType()->str().c_str());
			}

			for(auto block : ffn->getBlockList())
			{
				llvm::BasicBlock* bb = llvm::cast<llvm::BasicBlock>(valueMap[block->id]);
				builder.SetInsertPoint(bb);

				DUMP_INSTR("\n    %s", ("(%" + std::to_string(block->id) + ") " + block->getName() + ":\n").c_str());

				for(auto inst : block->getInstructions())
				{
					DUMP_INSTR("        %s\n", inst->str().c_str());

					// good god.
					switch(inst->opKind)
					{
						case fir::OpKind::Signed_Add:
						case fir::OpKind::Unsigned_Add:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateAdd(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Signed_Sub:
						case fir::OpKind::Unsigned_Sub:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateSub(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Signed_Mul:
						case fir::OpKind::Unsigned_Mul:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateMul(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Signed_Div:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateSDiv(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Signed_Mod:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateSRem(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Signed_Neg:
						{
							iceAssert(inst->operands.size() == 1);
							llvm::Value* a = getOperand(inst, 0);

							llvm::Value* ret = builder.CreateNeg(a);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Unsigned_Div:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateUDiv(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Unsigned_Mod:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateURem(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Add:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFAdd(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Sub:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFSub(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Mul:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFMul(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Div:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFDiv(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Mod:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFRem(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Neg:
						{
							iceAssert(inst->operands.size() == 1);
							llvm::Value* a = getOperand(inst, 0);

							llvm::Value* ret = builder.CreateFNeg(a);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Truncate:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();

							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreateFPTrunc(a, t);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Floating_Extend:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();

							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreateFPExt(a, t);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Integer_ZeroExt:
						case fir::OpKind::Integer_Truncate:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();

							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreateZExtOrTrunc(a, t);
							addValueToMap(ret, inst->realOutput);
							break;
						}




						case fir::OpKind::ICompare_Equal:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateICmpEQ(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::ICompare_NotEqual:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateICmpNE(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::ICompare_Greater:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = 0;
							if(inst->operands[0]->getType()->isSignedIntType() || inst->operands[1]->getType()->isSignedIntType())
								ret = builder.CreateICmpSGT(a, b);
							else
								ret = builder.CreateICmpUGT(a, b);

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::ICompare_Less:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = 0;
							if(inst->operands[0]->getType()->isSignedIntType() || inst->operands[1]->getType()->isSignedIntType())
								ret = builder.CreateICmpSLT(a, b);
							else
								ret = builder.CreateICmpULT(a, b);

							addValueToMap(ret, inst->realOutput);
							break;
						}


						case fir::OpKind::ICompare_GreaterEqual:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = 0;
							if(inst->operands[0]->getType()->isSignedIntType() || inst->operands[1]->getType()->isSignedIntType())
								ret = builder.CreateICmpSGE(a, b);
							else
								ret = builder.CreateICmpUGE(a, b);

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::ICompare_LessEqual:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = 0;
							if(inst->operands[0]->getType()->isSignedIntType() || inst->operands[1]->getType()->isSignedIntType())
								ret = builder.CreateICmpSLE(a, b);
							else
								ret = builder.CreateICmpULE(a, b);

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_Equal_ORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpOEQ(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_Equal_UNORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpUEQ(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_NotEqual_ORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpONE(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_NotEqual_UNORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpUNE(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_Greater_ORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpOGT(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_Greater_UNORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpUGT(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_Less_ORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpOLT(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_Less_UNORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpULT(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_GreaterEqual_ORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpOGE(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_GreaterEqual_UNORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpUGE(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_LessEqual_ORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpOLE(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::FCompare_LessEqual_UNORD:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateFCmpULE(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}


						case fir::OpKind::ICompare_Multi:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							bool sgn = inst->operands[0]->getType()->isSignedIntType() || inst->operands[1]->getType()->isSignedIntType();

							llvm::Value* r1 = 0;
							if(sgn)	r1 = builder.CreateICmpSGE(a, b);
							else	r1 = builder.CreateICmpUGE(a, b);

							llvm::Value* r2 = 0;
							if(sgn)	r2 = builder.CreateICmpSLE(a, b);
							else	r2 = builder.CreateICmpULE(a, b);

							r1 = builder.CreateIntCast(r1, llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), false);
							r2 = builder.CreateIntCast(r2, llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), false);

							llvm::Value* ret = builder.CreateSub(r1, r2);
							addValueToMap(ret, inst->realOutput);
							break;
						}


						case fir::OpKind::FCompare_Multi:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* r1 = builder.CreateFCmpOGE(a, b);
							llvm::Value* r2 = builder.CreateFCmpOLE(a, b);

							r1 = builder.CreateIntCast(r1, llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), false);
							r2 = builder.CreateIntCast(r2, llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()), false);

							llvm::Value* ret = builder.CreateSub(r1, r2);
							addValueToMap(ret, inst->realOutput);
							break;
						}
















						case fir::OpKind::Bitwise_Not:
						{
							iceAssert(inst->operands.size() == 1);
							llvm::Value* a = getOperand(inst, 0);

							llvm::Value* ret = builder.CreateNot(a);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Bitwise_Xor:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateXor(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Bitwise_Arithmetic_Shr:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateAShr(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Bitwise_Logical_Shr:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateLShr(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Bitwise_Shl:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateShl(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Bitwise_And:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateAnd(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Bitwise_Or:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateOr(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_Store:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							if(a->getType() != b->getType()->getPointerElementType())
							{
								error("cannot store %s into %s", inst->operands[0]->getType()->str().c_str(), inst->operands[1]->getType()->str().c_str());
							}
							llvm::Value* ret = builder.CreateStore(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_Load:
						{
							iceAssert(inst->operands.size() == 1);
							llvm::Value* a = getOperand(inst, 0);

							llvm::Value* ret = builder.CreateLoad(a);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_StackAlloc:
						{
							iceAssert(inst->operands.size() == 1);
							fir::Type* ft = inst->operands[0]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreateAlloca(t);
							builder.CreateStore(llvm::Constant::getNullValue(t), ret);

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_CallFunction:
						{
							iceAssert(inst->operands.size() >= 1);
							fir::Function* fn = dynamic_cast<fir::Function*>(inst->operands[0]);
							iceAssert(fn);

							llvm::Function* a = llvm::cast<llvm::Function>(getOperand(inst, 0));

							std::vector<llvm::Value*> args;

							std::vector<fir::Value*> fargs = inst->operands;

							for(size_t i = 1; i < fargs.size(); ++i)
								args.push_back(getValue(fargs[i]));

							llvm::Value* ret = builder.CreateCall(a, args);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_CallFunctionPointer:
						{
							iceAssert(inst->operands.size() >= 1);
							llvm::Value* fn = getOperand(inst, 0);

							std::vector<llvm::Value*> args;

							std::vector<fir::Value*> fargs = inst->operands;

							for(size_t i = 1; i < fargs.size(); ++i)
								args.push_back(getValue(fargs[i]));

							llvm::Type* lft = typeToLlvm(inst->operands.front()->getType(), module);

							iceAssert(lft->isPointerTy());
							iceAssert(lft->getPointerElementType()->isFunctionTy());

							llvm::FunctionType* ft = llvm::cast<llvm::FunctionType>(lft->getPointerElementType());
							iceAssert(ft);

							llvm::Value* ret = builder.CreateCall(ft, fn, args);

							addValueToMap(ret, inst->realOutput);

							break;
						}

						case fir::OpKind::Value_Return:
						{
							llvm::Value* ret = 0;
							if(inst->operands.size() == 0)
							{
								ret = builder.CreateRetVoid();
							}
							else
							{
								iceAssert(inst->operands.size() == 1);
								llvm::Value* a = getOperand(inst, 0);

								ret = builder.CreateRet(a);
							}

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Branch_UnCond:
						{
							iceAssert(inst->operands.size() == 1);
							llvm::Value* a = getOperand(inst, 0);

							llvm::Value* ret = builder.CreateBr(llvm::cast<llvm::BasicBlock>(a));
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Branch_Cond:
						{
							iceAssert(inst->operands.size() == 3);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);
							llvm::Value* c = getOperand(inst, 2);

							llvm::Value* ret = builder.CreateCondBr(a, llvm::cast<llvm::BasicBlock>(b), llvm::cast<llvm::BasicBlock>(c));
							addValueToMap(ret, inst->realOutput);
							break;
						}




						case fir::OpKind::Cast_Bitcast:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreateBitCast(a, t);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_IntSize:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);

							fir::Type* ft = inst->operands[1]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreateIntCast(a, t, ft->isSignedIntType());
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_Signedness:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);

							// no-op
							addValueToMap(a, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_FloatToInt:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = 0;
							if(ft->isSignedIntType())
								ret = builder.CreateFPToSI(a, t);
							else
								ret = builder.CreateFPToUI(a, t);

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_IntToFloat:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = 0;
							if(inst->operands[0]->getType()->isSignedIntType())
								ret = builder.CreateSIToFP(a, t);
							else
								ret = builder.CreateUIToFP(a, t);

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_PointerType:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreatePointerCast(a, t);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_PointerToInt:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreatePtrToInt(a, t);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_IntToPointer:
						{
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							fir::Type* ft = inst->operands[1]->getType();
							llvm::Type* t = typeToLlvm(ft, module);

							llvm::Value* ret = builder.CreateIntToPtr(a, t);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Cast_IntSignedness:
						{
							// is no op.
							// since llvm does not differentiate signed and unsigned.

							iceAssert(inst->operands.size() == 2);
							llvm::Value* ret = getOperand(inst, 0);

							addValueToMap(ret, inst->realOutput);
							break;
						}




						case fir::OpKind::Value_GetPointerToStructMember:
						{
							// equivalent to llvm's GEP(ptr*, ptrIndex, memberIndex)
							error("enotsup");
						}

						case fir::OpKind::Value_GetStructMember:
						{
							// equivalent to GEP(ptr*, 0, memberIndex)
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);

							fir::ConstantInt* ci = dynamic_cast<fir::ConstantInt*>(inst->operands[1]);
							iceAssert(ci);


							// todo: hack
							llvm::Value* ptr = a;
							// if(a->getType()->isPointerTy())
							// {
							// 	// nothing
							// }
							// else
							// {
							// 	ptr = builder.CreateAlloca(a->getType());
							// 	builder.CreateStore(a, ptr);
							// }

							llvm::Value* ret = builder.CreateStructGEP(ptr->getType()->getPointerElementType(), ptr, ci->getUnsignedValue());
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_GetPointer:
						{
							// equivalent to GEP(ptr*, index)
							iceAssert(inst->operands.size() == 2);
							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							llvm::Value* ret = builder.CreateGEP(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_GetGEP2:
						{
							// equivalent to GEP(ptr*, index)
							iceAssert(inst->operands.size() == 3);
							llvm::Value* a = getOperand(inst, 0);

							std::vector<llvm::Value*> indices = { getOperand(inst, 1), getOperand(inst, 2) };
							llvm::Value* ret = builder.CreateGEP(a, indices);

							addValueToMap(ret, inst->realOutput);
							break;
						}



						case fir::OpKind::Misc_Sizeof:
						{
							iceAssert(inst->operands.size() == 1);

							llvm::Type* t = getOperand(inst, 0)->getType();
							iceAssert(t);

							llvm::Value* gep = builder.CreateConstGEP1_64(llvm::ConstantPointerNull::get(t->getPointerTo()), 1);
							gep = builder.CreatePtrToInt(gep, llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()));

							addValueToMap(gep, inst->realOutput);
							break;
						}










						case fir::OpKind::Logical_Not:
						{
							iceAssert(inst->operands.size() == 1);
							llvm::Value* a = getOperand(inst, 0);

							llvm::Value* ret = builder.CreateICmpEQ(a, llvm::Constant::getNullValue(a->getType()));
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_PointerAddition:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(b->getType()->isIntegerTy());

							llvm::Value* ret = builder.CreateInBoundsGEP(a, b);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Value_PointerSubtraction:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(b->getType()->isIntegerTy());

							llvm::Value* negb = builder.CreateNeg(b);
							llvm::Value* ret = builder.CreateInBoundsGEP(a, negb);
							addValueToMap(ret, inst->realOutput);
							break;
						}




						case fir::OpKind::Value_InsertValue:
						{
							iceAssert(inst->operands.size() >= 3);

							llvm::Value* str = getOperand(inst, 0);
							llvm::Value* elm = getOperand(inst, 1);

							std::vector<unsigned int> inds;
							for(size_t i = 2; i < inst->operands.size(); i++)
							{
								fir::ConstantInt* ci = dynamic_cast<fir::ConstantInt*>(inst->operands[i]);
								iceAssert(ci);

								inds.push_back(ci->getUnsignedValue());
							}


							iceAssert(str->getType()->isStructTy() || str->getType()->isArrayTy());
							if(str->getType()->isStructTy())
							{
								iceAssert(elm->getType() == llvm::cast<llvm::StructType>(str->getType())->getElementType(inds[0]));
							}
							else if(str->getType()->isArrayTy())
							{
								iceAssert(elm->getType() == llvm::cast<llvm::ArrayType>(str->getType())->getElementType());
							}
							else
							{
								iceAssert(0);
							}

							llvm::Value* ret = builder.CreateInsertValue(str, elm, inds);
							addValueToMap(ret, inst->realOutput);

							break;
						}

						case fir::OpKind::Value_ExtractValue:
						{
							iceAssert(inst->operands.size() >= 2);

							llvm::Value* str = getOperand(inst, 0);

							std::vector<unsigned int> inds;
							for(size_t i = 1; i < inst->operands.size(); i++)
							{
								fir::ConstantInt* ci = dynamic_cast<fir::ConstantInt*>(inst->operands[i]);
								iceAssert(ci);

								inds.push_back(ci->getUnsignedValue());
							}

							iceAssert(str->getType()->isStructTy() || str->getType()->isArrayTy());

							llvm::Value* ret = builder.CreateExtractValue(str, inds);
							addValueToMap(ret, inst->realOutput);

							break;
						}


















						case fir::OpKind::String_GetData:
						case fir::OpKind::String_GetLength:
						{
							iceAssert(inst->operands.size() == 1);

							llvm::Value* a = getOperand(inst, 0);

							iceAssert(a->getType()->isStructTy());

							int ind = 0;
							if(inst->opKind == fir::OpKind::String_GetData)
								ind = 0;
							else if(inst->opKind == fir::OpKind::String_GetLength)
								ind = 1;

							llvm::Value* ret = builder.CreateExtractValue(a, ind);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::String_GetRefCount:
						{
							iceAssert(inst->operands.size() == 1);

							llvm::Value* a = getOperand(inst, 0);

							iceAssert(a->getType()->isStructTy());

							llvm::Value* dp = builder.CreateExtractValue(a, 0);

							// refcount lies 8 bytes behind.
							auto& gc = Compiler::LLVMBackend::getLLVMContext();
							llvm::Value* ptr = builder.CreatePointerCast(dp, llvm::Type::getInt64PtrTy(gc));
							ptr = builder.CreateInBoundsGEP(ptr, llvm::ConstantInt::getSigned(llvm::Type::getInt64Ty(gc), -1));

							llvm::Value* ret = builder.CreateLoad(ptr);
							addValueToMap(ret, inst->realOutput);
							break;
						}





						case fir::OpKind::String_SetData:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isStructTy());
							iceAssert(b->getType() == llvm::Type::getInt8PtrTy(Compiler::LLVMBackend::getLLVMContext()));

							llvm::Value* ret = builder.CreateInsertValue(a, b, 0);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::String_SetLength:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isStructTy());
							iceAssert(b->getType() == llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()));

							llvm::Value* ret = builder.CreateInsertValue(a, b, 1);
							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::String_SetRefCount:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isStructTy());
							iceAssert(b->getType() == llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()));

							llvm::Value* dp = builder.CreateExtractValue(a, 0);

							// refcount lies 8 bytes behind.
							auto& gc = Compiler::LLVMBackend::getLLVMContext();
							llvm::Value* ptr = builder.CreatePointerCast(dp, llvm::Type::getInt64PtrTy(gc));
							ptr = builder.CreateInBoundsGEP(ptr, llvm::ConstantInt::getSigned(llvm::Type::getInt64Ty(gc), -1));
							builder.CreateStore(b, ptr);

							llvm::Value* ret = builder.CreateLoad(ptr);
							addValueToMap(ret, inst->realOutput);
							break;
						}








						case fir::OpKind::DynamicArray_GetData:
						case fir::OpKind::DynamicArray_GetLength:
						case fir::OpKind::DynamicArray_GetCapacity:
						{
							iceAssert(inst->operands.size() == 1);

							llvm::Value* a = getOperand(inst, 0);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							int ind = 0;
							if(inst->opKind == fir::OpKind::DynamicArray_GetData)
								ind = 0;
							else if(inst->opKind == fir::OpKind::DynamicArray_GetLength)
								ind = 1;
							else
								ind = 2;

							llvm::Value* gep = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, ind);
							llvm::Value* ret = builder.CreateLoad(gep);
							addValueToMap(ret, inst->realOutput);
							break;
						}



						case fir::OpKind::DynamicArray_SetData:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							iceAssert(b->getType() == typeToLlvm(inst->operands[0]->getType()->getPointerElementType()->
								toDynamicArrayType()->getElementType()->getPointerTo(), module));

							llvm::Value* data = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, 0);
							builder.CreateStore(b, data);

							llvm::Value* ret = builder.CreateLoad(data);
							addValueToMap(ret, inst->realOutput);
							break;
						}


						case fir::OpKind::DynamicArray_SetLength:
						case fir::OpKind::DynamicArray_SetCapacity:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							iceAssert(b->getType() == llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()));

							int ind = 0;
							if(inst->opKind == fir::OpKind::DynamicArray_SetLength)
								ind = 1;
							else
								ind = 2;

							llvm::Value* len = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, ind);
							builder.CreateStore(b, len);

							llvm::Value* ret = builder.CreateLoad(len);
							addValueToMap(ret, inst->realOutput);
							break;
						}












						case fir::OpKind::ArraySlice_GetData:
						case fir::OpKind::ArraySlice_GetLength:
						{
							iceAssert(inst->operands.size() == 1);

							llvm::Value* a = getOperand(inst, 0);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							int ind = 0;

							if(inst->opKind == fir::OpKind::ArraySlice_GetLength)
								ind = 1;

							llvm::Value* gep = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, ind);
							llvm::Value* ret = builder.CreateLoad(gep);
							addValueToMap(ret, inst->realOutput);
							break;
						}



						case fir::OpKind::ArraySlice_SetData:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							iceAssert(b->getType() == typeToLlvm(inst->operands[0]->getType()->getPointerElementType()->
								toArraySliceType()->getElementType()->getPointerTo(), module));

							llvm::Value* data = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, 0);
							builder.CreateStore(b, data);

							llvm::Value* ret = builder.CreateLoad(data);
							addValueToMap(ret, inst->realOutput);
							break;
						}


						case fir::OpKind::ArraySlice_SetLength:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							iceAssert(b->getType() == llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()));

							llvm::Value* len = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, 1);
							builder.CreateStore(b, len);

							llvm::Value* ret = builder.CreateLoad(len);
							addValueToMap(ret, inst->realOutput);
							break;
						}









						case fir::OpKind::Any_GetData:
						{
							iceAssert(inst->operands.size() == 1);

							llvm::Value* a = getOperand(inst, 0);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							// get the array via GEP
							llvm::Value* data = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, 2);
							iceAssert(data->getType()->getPointerElementType()->isArrayTy());

							// get the pointer via GEP (again)
							llvm::Value* ret = builder.CreateConstGEP2_64(data, 0, 0);

							addValueToMap(ret, inst->realOutput);
							break;
						}

						case fir::OpKind::Any_SetData:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());
							iceAssert(module->getDataLayout().getTypeSizeInBits(b->getType()) <= 24 * CHAR_BIT);

							// get the pointer to array
							llvm::Value* data = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, 2);

							// get the real pointer
							llvm::Value* ptr = builder.CreateConstGEP2_64(data, 0, 0);

							// cast it
							ptr = builder.CreatePointerCast(ptr, b->getType()->getPointerTo());

							// store it
							builder.CreateStore(b, ptr);

							addValueToMap(builder.CreateLoad(ptr), inst->realOutput);
							break;
						}




						case fir::OpKind::Any_GetTypeID:
						case fir::OpKind::Any_GetFlag:
						{
							iceAssert(inst->operands.size() == 1);

							llvm::Value* a = getOperand(inst, 0);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							int ind = 0;
							if(inst->opKind == fir::OpKind::Any_GetTypeID)
								ind = 0;
							else
								ind = 1;

							llvm::Value* gep = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, ind);
							llvm::Value* ret = builder.CreateLoad(gep);

							addValueToMap(ret, inst->realOutput);
							break;
						}


						case fir::OpKind::Any_SetTypeID:
						case fir::OpKind::Any_SetFlag:
						{
							iceAssert(inst->operands.size() == 2);

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isPointerTy());
							iceAssert(a->getType()->getPointerElementType()->isStructTy());

							iceAssert(b->getType() == llvm::Type::getInt64Ty(Compiler::LLVMBackend::getLLVMContext()));

							int ind = 0;
							if(inst->opKind == fir::OpKind::Any_SetTypeID)
								ind = 0;
							else
								ind = 1;

							llvm::Value* len = builder.CreateStructGEP(a->getType()->getPointerElementType(), a, ind);
							builder.CreateStore(b, len);

							llvm::Value* ret = builder.CreateLoad(len);
							addValueToMap(ret, inst->realOutput);
							break;
						}



						case fir::OpKind::Range_GetLower:
						case fir::OpKind::Range_GetUpper:
						{
							unsigned int pos = 0;
							if(inst->opKind == fir::OpKind::Range_GetUpper)
								pos = 1;

							llvm::Value* a = getOperand(inst, 0);
							iceAssert(a->getType()->isStructTy());

							llvm::Value* val = builder.CreateExtractValue(a, { pos });
							addValueToMap(val, inst->realOutput);

							break;
						}


						case fir::OpKind::Range_SetLower:
						case fir::OpKind::Range_SetUpper:
						{
							unsigned int pos = 0;
							if(inst->opKind == fir::OpKind::Range_SetUpper)
								pos = 1;

							llvm::Value* a = getOperand(inst, 0);
							llvm::Value* b = getOperand(inst, 1);

							iceAssert(a->getType()->isStructTy());
							iceAssert(b->getType()->isIntegerTy());

							llvm::Value* ret = builder.CreateInsertValue(a, b, { pos });
							addValueToMap(ret, inst->realOutput);

							break;
						}






						case fir::OpKind::Unreachable:
						{
							builder.CreateUnreachable();
							break;
						}

						case fir::OpKind::Invalid:
						{
							// note we don't use "default" to catch
							// new opkinds that we forget to add.
							iceAssert("invalid opcode" && 0);
						}
					}
				}
			}
		}

		return module;
	}
}









































