// compiler.h
// Copyright (c) 2014 - 2015, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.

#pragma once

#include "ast.h"
#include <string>
#include <vector>
#include <map>


namespace llvm
{
	class Module;
}

namespace fir
{
	struct Module;
}

namespace Parser
{
	struct ParserState;
	struct Token;
}

namespace Codegen
{
	struct DepNode;
	struct DependencyGraph;
}

namespace Compiler
{
	struct CompiledData
	{
		Ast::Root* rootNode = 0;

		std::unordered_map<std::string, Ast::Root*> rootMap;
		std::deque<std::pair<std::string, fir::Module*>> moduleList;


		fir::Module* getModule(std::string name)
		{
			for(auto pair : this->moduleList)
			{
				if(pair.first == name)
					return pair.second;
			}

			return 0;
		}
	};

	std::deque<std::deque<Codegen::DepNode*>> checkCyclicDependencies(std::string filename);

	CompiledData compileFile(std::string filename,std::deque<std::deque<Codegen::DepNode*>> groups,
		std::map<Ast::ArithmeticOp, std::pair<std::string, int>> foundOps, std::map<std::string, Ast::ArithmeticOp> foundOpsRev);

	std::string resolveImport(Ast::Import* imp, std::string fullPath);


	std::deque<Parser::Token> getFileTokens(std::string fullPath);
	std::vector<std::string> getFileLines(std::string fullPath);
	std::string getFileContents(std::string fullPath);

	std::string getPathFromFile(std::string path);
	std::string getFilenameFromPath(std::string path);
	std::string getFullPathOfFile(std::string partial);

	std::pair<std::string, std::string> parseCmdLineArgs(int argc, char** argv);


	bool getDumpFir();
	bool getDumpLlvm();
	bool getEmitLLVMOutput();
	bool showProfilerOutput();
	std::string getTarget();
	std::string getPrefix();
	std::string getCodeModel();
	std::string getSysroot();

	std::deque<std::string> getLibrarySearchPaths();
	std::deque<std::string> getLibrariesToLink();
	std::deque<std::string> getFrameworksToLink();
	std::deque<std::string> getFrameworkSearchPaths();

	enum class BackendOption;
	enum class OptimisationLevel;
	enum class ProgOutputMode;


	ProgOutputMode getOutputMode();
	BackendOption getSelectedBackend();
	OptimisationLevel getOptimisationLevel();

	bool getPrintClangOutput();
	bool getIsPositionIndependent();
	bool getNoAutoGlobalConstructor();


	enum class Flag
	{
		WarningsAsErrors		= 0x1,
		NoWarnings				= 0x2,
	};

	enum class Warning
	{
		UnusedVariable,
		UseBeforeAssign,
		UseAfterFree,
	};

	bool getFlag(Flag f);
	bool getWarningEnabled(Warning warning);
}







