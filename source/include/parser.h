// parser.h
// Copyright (c) 2014 - The Foreseeable Future, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.

#pragma once


#include "ast.h"
#include <string>
#include <sstream>

namespace Codegen
{
	struct CodegenInstance;
}

namespace Ast
{
	struct Root;
	enum class ArithmeticOp;
}


namespace Parser
{
	enum class TType
	{
		// keywords
		Func,
		Class,
		Import,
		Var,
		Val,
		Ptr,
		Deref,
		Addr,
		If,
		Else,
		Return,
		As,
		Is,
		Switch,
		Case,
		Match,
		To,
		Enum,
		ForeignFunc,
		Struct,
		True,
		False,

		For,
		While,
		Do,
		Loop,

		Break,
		Continue,

		Get,
		Set,

		Private,
		Public,
		Internal,

		Extension,
		TypeAlias,

		BuiltinType,
		Alloc,
		Dealloc,

		Module,
		Namespace,


		// symbols
		LBrace,
		RBrace,
		LParen,
		RParen,
		LSquare,
		RSquare,
		LAngle,
		RAngle,
		Plus,
		Minus,
		Asterisk,
		Divide,
		SQuote,
		DQuote,
		Period,
		Comma,
		Colon,
		Equal,
		Question,
		Exclamation,
		Semicolon,
		Ampersand,
		Percent,
		Pipe,
		LogicalOr,
		LogicalAnd,
		At,
		Pound,
		Tilde,


		// compound symbols
		Arrow,
		EqualsTo,
		NotEquals,
		GreaterEquals,
		LessThanEquals,
		ShiftLeft,
		ShiftRight,
		DoublePlus,			// doubleplusgood
		DoubleMinus,
		PlusEq,
		MinusEq,
		MultiplyEq,
		DivideEq,
		ModEq,
		ShiftLeftEq,
		ShiftRightEq,
		Elipsis,
		DoubleColon,

		// other stuff.
		Identifier,
		Integer,
		Decimal,
		StringLiteral,

		NewLine,
		Comment,
	};


	struct Token
	{
		PosInfo posinfo;
		std::string text;
		TType type;
	};


	void parserError(const char* msg, ...) __attribute__((noreturn));
	void parserWarn(const char* msg, ...);



	typedef std::deque<Token> TokenList;





	void parseAll(TokenList& tokens);
	Ast::Expr* parsePrimary(TokenList& tokens);

	Ast::Expr* parseIf(TokenList& tokens);
	void parseAttribute(TokenList& tokens);
	Ast::Func* parseFunc(TokenList& tokens);
	Ast::Expr* parseExpr(TokenList& tokens);
	Ast::Expr* parseUnary(TokenList& tokens);
	Ast::Alloc* parseAlloc(TokenList& tokens);
	Ast::ForLoop* parseFor(TokenList& tokens);
	Ast::Expr* parseIdExpr(TokenList& tokens);
	Ast::Break* parseBreak(TokenList& tokens);
	Ast::Expr* parseInitFunc(TokenList& tokens);
	Ast::Struct* parseStruct(TokenList& tokens);
	Ast::Import* parseImport(TokenList& tokens);
	Ast::Return* parseReturn(TokenList& tokens);
	Ast::Number* parseNumber(TokenList& tokens);
	Ast::CastedType* parseType(TokenList& tokens);
	Ast::VarDecl* parseVarDecl(TokenList& tokens);
	Ast::WhileLoop* parseWhile(TokenList& tokens);
	Ast::Dealloc* parseDealloc(TokenList& tokens);
	Ast::Enumeration* parseEnum(TokenList& tokens);
	Ast::Continue* parseContinue(TokenList& tokens);
	Ast::Func* parseTopLevelExpr(TokenList& tokens);
	Ast::FuncDecl* parseFuncDecl(TokenList& tokens);
	Ast::Expr* parseParenthesised(TokenList& tokens);
	Ast::TypeAlias* parseTypeAlias(TokenList& tokens);
	Ast::Extension* parseExtension(TokenList& tokens);
	Ast::OpOverload* parseOpOverload(TokenList& tokens);
	Ast::NamespaceDecl* parseNamespace(TokenList& tokens);
	Ast::BracedBlock* parseBracedBlock(TokenList& tokens);
	Ast::StringLiteral* parseStringLiteral(TokenList& tokens);
	Ast::ForeignFuncDecl* parseForeignFunc(TokenList& tokens);
	Ast::Expr* parseFuncCall(TokenList& tokens, std::string id);
	Ast::Expr* parseRhs(TokenList& tokens, Ast::Expr* expr, int prio);












	std::string getModuleName(std::string filename);
	Ast::Root* Parse(std::string filename, std::string str, Codegen::CodegenInstance* cgi);
	Token getNextToken(std::string& stream, PosInfo& pos);
	std::string arithmeticOpToString(Ast::ArithmeticOp op);

	Ast::ArithmeticOp mangledStringToOperator(std::string op);
	std::string operatorToMangledString(Ast::ArithmeticOp op);
}






