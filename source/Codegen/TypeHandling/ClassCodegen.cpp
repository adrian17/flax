// ClassCodegen.cpp
// Copyright (c) 2014 - 2015, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.


#include "ast.h"
#include "codegen.h"

#include "classbase.h"

using namespace Ast;
using namespace Codegen;



fir::Type* ClassDef::getType(CodegenInstance* cgi, fir::Type* extratype, bool allowFail)
{
	if(this->createdType == 0)
		return this->createType(cgi);

	else return this->createdType;
}

fir::Type* ClassDef::reifyTypeUsingMapping(CodegenInstance* cgi, Expr* user, std::map<std::string, fir::Type*> tm)
{
	if(cgi->reifiedGenericTypes.find({ this, tm }) != cgi->reifiedGenericTypes.end())
		return cgi->reifiedGenericTypes[{ this, tm }];

	{
		std::vector<std::string> needed;
		for(auto t : this->genericTypes)
			needed.push_back(t.first);

		for(auto n : needed)
		{
			if(tm.find(n) == tm.end())
			{
				error(user, "Missing type parameter for generic type '%s' in instantiation of class '%s'",
					n.c_str(), this->ident.name.c_str());
			}
		}

		for(auto t : tm)
		{
			if(std::find(needed.begin(), needed.end(), t.first) == needed.end())
				error(user, "Extraneous type parameter '%s' that does not exist in class '%s'", t.first.c_str(), this->ident.name.c_str());
		}
	}




	cgi->pushGenericTypeMap(tm);

	TypePair_t* _type = cgi->getType(this->ident);
	if(!_type) error(this, "how? generating class (%s) without type", this->ident.name.c_str());


	fir::LinkageType linkageType;
	if(this->attribs & Attr_VisPublic)
	{
		linkageType = fir::LinkageType::External;
	}
	else
	{
		linkageType = fir::LinkageType::Internal;
	}






	// see if we have nested types
	for(auto nested : this->nestedTypes)
	{
		cgi->pushNestedTypeScope(this);
		cgi->pushNamespaceScope(this->ident.name);

		nested.first->codegen(cgi);

		cgi->popNamespaceScope();
		cgi->popNestedTypeScope();
	}


	fir::ClassType* cls = this->createdType->toClassType();
	cls = cls->reify(tm);

	cgi->module->addNamedType(cls->getClassName(), cls);

	// add the concrete type to the mapping as well.
	if(this->genericTypes.size() > 0)
		cgi->addNewType(cls, this, TypeKind::Class);


	fir::Function* initFunction = 0;

	// generate initialiser
	{
		auto defaultInitId = this->ident;
		defaultInitId.kind = IdKind::AutoGenFunc;
		defaultInitId.name = "init_" + defaultInitId.name;
		defaultInitId.functionArguments = { cls->getPointerTo() };

		initFunction = cgi->module->getOrCreateFunction(defaultInitId, fir::FunctionType::get({ cls->getPointerTo() },
			fir::Type::getVoid(cgi->getContext()), false), linkageType);


		fir::IRBlock* currentblock = cgi->irb.getCurrentBlock();

		fir::IRBlock* iblock = cgi->irb.addNewBlockInFunction("initialiser_" + this->ident.name, initFunction);
		cgi->irb.setCurrentBlock(iblock);

		// create the local instance of reference to self
		fir::Value* self = initFunction->getArguments().front();

		for(VarDecl* var : this->members)
		{
			if(!var->isStatic)
			{
				fir::Value* ptr = cgi->irb.CreateGetStructMember(self, var->ident.name);

				auto r = var->initVal ? var->initVal->codegen(cgi) : Result_t(0, 0);

				var->inferType(cgi);
				var->doInitialValue(cgi, cgi->getType(var->concretisedType), r.value, r.pointer, ptr, false, r.valueKind);
			}
			else
			{
				// generate some globals for static variables.

				auto tmp = this->ident.scope;
				tmp.push_back(this->ident.name);

				Identifier vid = Identifier(var->ident.name, tmp, IdKind::Variable);

				// generate a global variable
				fir::GlobalVariable* gv = cgi->module->createGlobalVariable(vid, var->concretisedType,
					fir::ConstantValue::getZeroValue(var->concretisedType), var->immutable,
					(this->attribs & Attr_VisPublic) ? fir::LinkageType::External : fir::LinkageType::Internal);

				if(var->concretisedType->isStructType() || var->concretisedType->isClassType())
				{
					TypePair_t* cmplxtype = cgi->getType(var->concretisedType);
					iceAssert(cmplxtype);

					fir::Function* init = cgi->getStructInitialiser(var, cmplxtype, { gv }, { }, var->ptype);
					cgi->addGlobalConstructor(vid, init);
				}
				else
				{
					iceAssert(var->initVal);
					fir::Value* val = var->initVal->codegen(cgi, var->concretisedType, gv).value;
					if(dynamic_cast<fir::ConstantValue*>(val))
					{
						gv->setInitialValue(dynamic_cast<fir::ConstantValue*>(cgi->autoCastType(var->concretisedType, val)));
					}
					else
					{
						error(this, "Static variables currently only support constant initialisers");
					}
				}
			}
		}

		cgi->irb.CreateReturnVoid();
		cgi->irb.setCurrentBlock(currentblock);
	}

	// generate the decls before the bodies, so we can (a) call recursively, and (b) call other member functions independent of
	// order of declaration.


	// pass 1
	auto fmap = doCodegenForMemberFunctions(cgi, this, cls, tm);
	{
		cls->setMethods(this->lfuncs);
	}


	// do comprops here:
	// 1. we need to generate the decls separately (because they're fake)
	// 2. we need to *get* the fir::Function* to store somewhere to retrieve later
	// 3. we need the rest of the member decls to be in place, so we can call member functions
	// from the getters/setters.
	doCodegenForComputedProperties(cgi, this, cls, tm);

	// same reasoning for operators -- we need to 1. be able to call methods in the operator, and 2. call operators from the methods
	generateDeclForOperators(cgi, this, cls, tm);

	doCodegenForGeneralOperators(cgi, this, cls, tm);
	doCodegenForAssignmentOperators(cgi, this, cls, tm);
	doCodegenForSubscriptOperators(cgi, this, cls, tm);


	// pass 2
	for(Func* f : this->funcs)
	{
		generateMemberFunctionBody(cgi, this, cls, f, initFunction, fmap[f], tm);
	}









	for(auto protstr : this->protocolstrs)
	{
		ProtocolDef* prot = cgi->resolveProtocolName(this, protstr);
		iceAssert(prot);

		prot->assertTypeConformityWithErrorMessage(cgi, this, cls, strprintf("Class '%s' declared conformity to protocol '%s', but does not conform to it", this->ident.name.c_str(), protstr.c_str()));

		this->conformedProtocols.push_back(prot);
	}







	// if(initFuncs.size() == 0 && initFunction != 0)
	// {
	// 	this->initFuncs.push_back(initFunction);
	// }
	// else
	// {
	// 	// handles generic types making more default initialisers

		bool found = false;
		for(auto f : initFuncs)
		{
			if(f->getType() == initFunction->getType())
			{
				found = true;
				break;
			}
		}

		if(!found) this->initFuncs.push_back(initFunction);
	// }

	cgi->popGenericTypeStack();
	cgi->reifiedGenericTypes[{ this, tm }] = cls;

	return cls;
}

Result_t ClassDef::codegen(CodegenInstance* cgi, fir::Type* extratype, fir::Value* target)
{
	if(!this->createdType)
		this->createType(cgi);

	if(this->genericTypes.size() == 0)
		this->reifyTypeUsingMapping(cgi, this, { });

	return Result_t(0, 0);
}































fir::Type* ClassDef::createType(CodegenInstance* cgi)
{
	if(this->didCreateType && this->genericTypes.empty())
		return this->createdType;


	cgi->pushGenericTypeStack();
	std::vector<fir::ParametricType*> typeParams;
	if(this->genericTypes.size() > 0)
	{
		for(auto t : this->genericTypes)
		{
			auto pt = fir::ParametricType::get(t.first);
			cgi->pushGenericType(t.first, pt);
			typeParams.push_back(pt);
		}
	}



	// see if we have nested types
	for(auto nested : this->nestedTypes)
	{
		cgi->pushNestedTypeScope(this);
		cgi->pushNamespaceScope(this->ident.name);

		nested.second = nested.first->createType(cgi);

		cgi->popNamespaceScope();
		cgi->popNestedTypeScope();
	}



	std::vector<std::pair<std::string, fir::Type*>> types;



	// create a bodyless struct so we can use it

	if(cgi->isDuplicateType(this->ident))
		GenError::duplicateSymbol(cgi, this, this->ident.str(), SymbolType::Type);

	fir::ClassType* cls = fir::ClassType::createWithoutBody(this->ident, cgi->getContext());
	cls->addTypeParameters(typeParams);

	iceAssert(this->createdType == 0);
	cgi->addNewType(cls, this, TypeKind::Class);





	for(Func* func : this->funcs)
	{
		// only override if we don't have one.
		if(this->attribs & Attr_VisPublic && !(func->decl->attribs & (Attr_VisInternal | Attr_VisPrivate | Attr_VisPublic)))
			func->decl->attribs |= Attr_VisPublic;

		func->decl->parentClass = { this, cls };
	}

	for(VarDecl* var : this->members)
	{
		var->inferType(cgi);
		iceAssert(var->concretisedType != 0);

		fir::Type* type = var->concretisedType;

		if(type == cls)
		{
			error(var, "Cannot have non-pointer member of type self");
		}

		if(!var->isStatic)
		{
			types.push_back({ var->ident.name, var->getType(cgi) });
		}
	}

	cls->setMembers(types);

	this->didCreateType = true;
	this->createdType = cls;

	cgi->popGenericTypeStack();
	return cls;
}




















