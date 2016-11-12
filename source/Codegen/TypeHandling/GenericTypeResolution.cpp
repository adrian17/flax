// GenericTypeResolution.cpp
// Copyright (c) 2014 - 2016, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.

#include "pts.h"
#include "ast.h"
#include "codegen.h"

using namespace Ast;

namespace Codegen
{
	static std::string _makeErrorString(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	static std::string _makeErrorString(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);

		char* tmp = 0;
		vasprintf(&tmp, fmt, ap);

		std::string ret = tmp;
		free(tmp);

		va_end(ap);
		return ret;
	}



	static void _getAllGenericTypesContainedWithinRecursively(pts::Type* t, const std::map<std::string, TypeConstraints_t>& gt,
		std::deque<std::string>* list)
	{
		if(t->isNamedType())
		{
			if(gt.find(t->toNamedType()->name) != gt.end())
				list->push_back(t->toNamedType()->name);
		}
		else if(t->isFunctionType())
		{
			for(auto p : t->toFunctionType()->argTypes)
				_getAllGenericTypesContainedWithinRecursively(p, gt, list);

			_getAllGenericTypesContainedWithinRecursively(t->toFunctionType()->returnType, gt, list);
		}
		else if(t->isTupleType())
		{
			for(auto m : t->toTupleType()->types)
				_getAllGenericTypesContainedWithinRecursively(m, gt, list);
		}
		else if(t->isPointerType())
		{
			while(t->isPointerType())
				t = t->toPointerType()->base;

			_getAllGenericTypesContainedWithinRecursively(t, gt, list);
		}
		else if(t->isDynamicArrayType())
		{
			_getAllGenericTypesContainedWithinRecursively(t->toDynamicArrayType()->base, gt, list);
		}
		else if(t->isFixedArrayType())
		{
			_getAllGenericTypesContainedWithinRecursively(t->toFixedArrayType()->base, gt, list);
		}
		else if(t->isVariadicArrayType())
		{
			_getAllGenericTypesContainedWithinRecursively(t->toVariadicArrayType()->base, gt, list);
		}
		else
		{
			iceAssert("??" && 0);
		}
	}



	static std::deque<std::string> getAllGenericTypesContainedWithin(pts::Type* t, const std::map<std::string, TypeConstraints_t>& gt)
	{
		std::deque<std::string> ret;
		_getAllGenericTypesContainedWithinRecursively(t, gt, &ret);

		return ret;
	}

















	// solver for function types
	// accepts a parameter, returns whether a solution was found (bool)
	static bool checkGenericFunctionOrTupleArgument(CodegenInstance* cgi, FuncDecl* candidate, size_t ix, pts::Type* prm, fir::Type* arg,
		std::map<std::string, fir::Type*>* resolved, std::string* errorString, Expr** failedExpr)
	{
		typedef std::deque<pts::TypeTransformer> TrfList;

		// decompose each type fully
		pts::Type* dpt = 0; TrfList ptrfs;
		std::tie(dpt, ptrfs) = pts::decomposeTypeIntoBaseTypeWithTransformations(prm);

		fir::Type* dft = 0; TrfList ftrfs;
		std::tie(dft, ftrfs) = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(arg);

		iceAssert(dpt->isFunctionType() || dpt->isTupleType());
		if(!pts::areTransformationsCompatible(ptrfs, ftrfs))
		{
			if(errorString && failedExpr)
			{
				*errorString = _makeErrorString("Incompatible types in argument %zu: expected '%s', have '%s' (No valid transformations)",
					ix + 1, prm->str().c_str(), arg->str().c_str());

				*failedExpr = candidate->params[ix];
			}

			return false;
		}
		else if((!dft->isFunctionType() && !dft->isTupleType()) || (dft->isFunctionType() && !dpt->isFunctionType())
			|| (dft->isTupleType() && !dpt->isTupleType()))
		{
			if(errorString && failedExpr)
			{
				*errorString = _makeErrorString("Incompatible types in solution for argument %zu:"
					" expected %s type '%s', have '%s'", ix + 1, dpt->isFunctionType() ? "function" : "tuple",
					prm->str().c_str(), arg->str().c_str());

				*failedExpr = candidate->params[ix];
			}

			return false;
		}



		// ok, the types are compatible
		// look at arguments

		// note: should you be able to pass a function (T, K[...]) -> void to an argument taking (T) -> void?
		// it shouldn't make a difference, but would probably be unexpected.

		std::deque<fir::Type*> ftlist;
		std::deque<pts::Type*> ptlist;
		if(dft->isFunctionType())
		{
			ftlist = dft->toFunctionType()->getArgumentTypes();
			ptlist = dpt->toFunctionType()->argTypes;
		}
		else
		{
			ftlist = std::deque<fir::Type*>(dft->toTupleType()->getElements().begin(), dft->toTupleType()->getElements().end());
			ptlist = dpt->toTupleType()->types;
		}

		if(ftlist.size() != ptlist.size())
		{
			if(errorString && failedExpr)
			{
				*errorString = _makeErrorString("Incompatible (function or tuple) type lists in argument %zu:"
					" Size mismatch, have %zu, expected %zu", ix + 1, ftlist.size(), ptlist.size());
				*failedExpr = candidate->params[ix];
			}
			return false;
		}


		std::deque<std::string> toSolve = getAllGenericTypesContainedWithin(dpt, candidate->genericTypes);








		// infinite loop
		for(size_t cnt = 0;; cnt++)
		{
			// save a local copy of the current soluion
			auto cursln = *resolved;

			// ok, now... loop through each item
			for(size_t i = 0; i < ftlist.size(); i++)
			{
				// check the given with the expected
				fir::Type* givent = 0; TrfList giventrfs;
				std::tie(givent, giventrfs) = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(ftlist[i]);

				pts::Type* expt = 0; TrfList exptrfs;
				std::tie(expt, exptrfs) = pts::decomposeTypeIntoBaseTypeWithTransformations(ptlist[i]);

				if(!pts::areTransformationsCompatible(exptrfs, giventrfs))
				{
					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("Incompatible types in solution for argument %zu of function parameter"
							" (which is argument %zu of parent function): expected '%s', have '%s' (No valid transformations)",
							i + 1, ix + 1, expt->str().c_str(), arg->str().c_str());

						*failedExpr = candidate->params[ix];
					}
					return false;
				}


				// check if a solution for this type already exists
				if(expt->isNamedType())
				{
					if(cursln.find(expt->toNamedType()->name) != cursln.end())
					{
						// yes it does
						fir::Type* rest = cursln[expt->toNamedType()->name];
						iceAssert(rest);


						// if our counterpart is *also* a parametric type, use this as the solution
						// if not, check that the concrete types match.
						if(givent->isParametricType())
						{
							// hmm
							error("no");

							// we must remember that we can't compare names
							// T in this function is completely different from T in another function

							// in this branch, 'rest' is a valid type that we have previously resolved.

						}
						else
						{
							if(rest != givent)
							{
								if(errorString && failedExpr)
								{
									*errorString = _makeErrorString("Conflicting types in solution for type parameter '%s', in argument %zu of function parameter (which is argument %zu of parent function): have existing solution '%s', found '%s'",
										expt->toNamedType()->name.c_str(), i + 1, ix + 1, rest->str().c_str(), givent->str().c_str());

									*failedExpr = candidate->params[ix];
								}
								return false;
							}
							else
							{
								// ok
							}
						}
					}
					else
					{
						// nevermind.
						info("no solution yet??");
					}
				}
				else
				{
					error("notsup nested function");
				}
			}


			// try the return type if this is a function
			if(dft->isFunctionType())
			{
				iceAssert(dpt->isFunctionType());

				// check the given with the expected
				fir::Type* ft = 0; TrfList fttrfs;
				std::tie(ft, fttrfs) = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(dft->toFunctionType()->getReturnType());

				pts::Type* pt = 0; TrfList pttrfs;
				std::tie(pt, pttrfs) = pts::decomposeTypeIntoBaseTypeWithTransformations(dpt->toFunctionType()->returnType);


				// check if we don't already have a solution for 'pt'.
				if(pt->isNamedType())
				{
					if(ft->isParametricType())
					{
						error("no 2");
					}
					else
					{
						if(cursln.find(pt->toNamedType()->name) != cursln.end())
						{
							// check if they conflict
							fir::Type* rest = cursln[pt->toNamedType()->name];
							iceAssert(rest);

							if(rest != ft)
							{
								if(errorString && failedExpr)
								{
									*errorString = _makeErrorString("Conflicting types in solution for type parameter '%s', in return type"
										" function parameter (which is argument %zu of parent function): have existing solution '%s',"
										" found '%s'", pt->toNamedType()->name.c_str(), ix + 1, rest->str().c_str(), ft->str().c_str());

									*failedExpr = candidate->params[ix];
								}
								return false;
							}
							else
							{
								// ok
							}
						}
						else
						{
							// hmm.
							// use the return type directly
							// we know here it's not a parametric type, so it's ok.

							// no solution, so this *is* the solution
							cursln[pt->toNamedType()->name] = ft;
						}
					}
				}
				else
				{
					error("notsup nested things");
				}
			}



			// if our local copy matches the resolved copy, we made no progress this time.
			if(*resolved == cursln)
			{
				// check if we've solved everything, if so then we can return true.
				for(auto s : toSolve)
				{
					if(cursln.find(s) == cursln.end())
					{
						if(errorString && failedExpr)
						{
							std::string solvedstr; // = "\nSolutions found so far: ";
							for(auto t : cursln)
								solvedstr += _makeErrorString("'%s': '%s', ", t.first.c_str(), t.second->str().c_str());

							if(solvedstr.empty())
							{
								solvedstr = "\n(no partial solutions found)";
							}
							else
							{
								solvedstr = "\nPartial solutions: " + solvedstr;
								solvedstr = solvedstr.substr(solvedstr.length() - 2);
							}

							*errorString = _makeErrorString("Failed to find solution for function parameter %zu in parent function using"
								" provided type '%s' to solve '%s'; made no progress after %zu iterations, and terminated.%s",
								ix, arg->str().c_str(), prm->str().c_str(), cnt, solvedstr.c_str());

							*failedExpr = candidate->params[ix];
						}
						return false;
					}
				}

				*resolved = cursln;
				return true;
			}

			cnt++;
			*resolved = cursln;
		}

		return true;
	}


















	// main solver function
	static bool checkGenericFunction(CodegenInstance* cgi, std::map<std::string, fir::Type*>* gtm,
		FuncDecl* candidate, std::deque<fir::Type*> args, std::string* errorString, Expr** failedExpr)
	{
		// get rid of this stupid literal nonsense
		for(size_t i = 0; i < args.size(); i++)
		{
			if(args[i]->isPrimitiveType() && args[i]->toPrimitiveType()->isLiteralType())
				args[i] = args[i]->toPrimitiveType()->getUnliteralType();
		}


		if(candidate->params.size() != args.size())
		{
			// if it's not variadic, and it's either a normal function (no parent class) or is a static method,
			// then there's no reason for the parameters to mismatch.
			if(!candidate->isVariadic && (!candidate->parentClass || candidate->isStatic))
			{
				if(errorString && failedExpr)
				{
					*errorString = _makeErrorString("Mismatched argument count; expected %zu, have %zu",
						candidate->params.size(), args.size());

					*failedExpr = candidate;
				}
				return false;
			}
			else if(candidate->parentClass && !candidate->isStatic)
			{
				// make sure it's only one off
				if(args.size() < candidate->params.size() || args.size() - candidate->params.size() > 1)
				{
					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("Mismatched argument count; expected %zu, have %zu",
							candidate->params.size(), args.size());

						*failedExpr = candidate;
					}
					return false;
				}

				// didSelfParam = true;
				iceAssert(args.front()->isPointerType() && args.front()->getPointerElementType()->isClassType() && "what, no.");

				// originalFirstParam = args.front();
				args.pop_front();

				iceAssert(candidate->params.size() == args.size());
			}
		}


		// if the candidate is variadic, the number of parameters must be *at least* the number of fixed parameters
		VarDecl* varParam = 0;
		if(candidate->isVariadic)
		{
			if(args.size() < candidate->params.size() - 1)
			{
				if(errorString && failedExpr)
				{
					*errorString = _makeErrorString("Mismatched argument count; expected at least %zu, have %zu",
						candidate->params.size() - 1, args.size());

					*failedExpr = candidate;
				}
				return false;
			}

			// if this is variadic, remove the last parameter from the candidate (we'll add it back later)
			// so we only check the first n - 1 parameters.

			varParam = candidate->params.back();
			candidate->params.pop_back();
		}


		std::map<std::string, std::deque<std::pair<size_t, fir::Type*>>> candidateGenerics;
		std::map<size_t, std::string> genericPositions;
		std::map<size_t, fir::Type*> nonGenericTypes;

		std::map<std::string, std::deque<size_t>> genericPositions2;

		for(size_t i = 0; i < candidate->params.size(); i++)
		{
			pts::Type* pt = 0; std::deque<pts::TypeTransformer> trfs;
			std::tie(pt, trfs) = pts::decomposeTypeIntoBaseTypeWithTransformations(candidate->params[i]->ptype);

			// maximally solve all the trivial types first.
			if(pt->isNamedType())
			{
				if(candidate->genericTypes.find(pt->toNamedType()->name) != candidate->genericTypes.end())
				{
					// a bit cumbersome, but whatever
					candidateGenerics[pt->toNamedType()->name].push_back({ i, args[i] });
					genericPositions[i] = pt->toNamedType()->name;
				}
				else
				{
					fir::Type* resolved = cgi->getTypeFromParserType(candidate, pt);
					iceAssert(resolved);

					nonGenericTypes[i] = pts::applyTransformationsOnType(resolved, trfs);
				}
			}
			else if(pt->isFunctionType() || pt->isTupleType())
			{
				auto l = getAllGenericTypesContainedWithin(pt, candidate->genericTypes);
				for(auto s : l)
					genericPositions2[s].push_back(i);
			}
			else
			{
				error("??");
			}
		}



		// first check non-generic types
		for(auto t : nonGenericTypes)
		{
			iceAssert(t.first < args.size());
			fir::Type* a = t.second;
			fir::Type* b = args[t.first];

			if(a != b && cgi->getAutoCastDistance(b, a) == -1)
			{
				if(errorString && failedExpr)
				{
					*errorString = _makeErrorString("No conversion from given type '%s' to expected type '%s' in argument %zu",
						b->str().c_str(), a->str().c_str(), t.first + 1);
					*failedExpr = candidate->params[t.first];
				}
				return false;
			}
		}



		// ok, now check the generic types
		std::map<std::string, fir::Type*> thistm;

		for(auto gt : candidateGenerics)
		{
			// first make sure all the types match up
			fir::Type* res = 0;
			fir::Type* baseres = 0;
			for(auto t : gt.second)
			{
				fir::Type* base = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(t.second).first;

				if(baseres == 0)
				{
					baseres = base;
					res = t.second;
					continue;
				}
				else if(base != baseres)
				{
					// no casting here, generic types need to be exact.

					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("Conflicting solutions for parametric type '%s' in argument %zu: '%s' and '%s'",
							gt.first.c_str(), t.first + 1, res->str().c_str(), t.second->str().c_str());
						*failedExpr = candidate->params[t.first];
					}
					return false;
				}
			}


			// loop again
			fir::Type* sol = 0;
			for(auto t : gt.second)
			{
				size_t i = t.first;

				// check that the transformations are compatible
				// eg. we don't try to pass a parameter T to the function expecting K** -- even if the base types match
				// eg. passing int to int**

				fir::Type* arg = args[i]; fir::Type* _ = 0; std::deque<pts::TypeTransformer> argtrfs;
				std::tie(_, argtrfs) = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(arg);

				pts::Type* g = 0; std::deque<pts::TypeTransformer> gtrfs;
				std::tie(g, gtrfs) = pts::decomposeTypeIntoBaseTypeWithTransformations(candidate->params[i]->ptype);

				if(!pts::areTransformationsCompatible(gtrfs, argtrfs))
				{
					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("Incompatible types in solution for parametric type '%s' in argument %zu:"
							" expected '%s', have '%s' (No valid transformations)", genericPositions[i].c_str(), i + 1,
							candidate->params[i]->ptype->str().c_str(), args[i]->str().c_str());

						*failedExpr = candidate->params[i];
					}
					return false;
				}

				fir::Type* soln = pts::reduceMaximallyWithSubset(arg, gtrfs, argtrfs);
				if(sol == 0)
				{
					sol = soln;
				}
				else if(sol != soln)
				{
					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("Incompatible types in solution for parametric type '%s' in argument %zu:"
							" expected '%s', have '%s' (Previously '%s' was solved as '%s')", genericPositions[i].c_str(), i + 1,
							candidate->params[i]->ptype->str().c_str(), args[i]->str().c_str(), genericPositions[i].c_str(),
							sol->str().c_str());

						*failedExpr = candidate->params[i];
					}
					return false;
				}
			}

			iceAssert(sol);
			thistm[gt.first] = sol;
		}


		// check variadic

		if(varParam != 0)
		{
			// add it back first.
			candidate->params.push_back(varParam);

			// get the type.
			pts::Type* pt = 0; std::deque<pts::TypeTransformer> _trfs;
			std::tie(pt, _trfs) = pts::decomposeTypeIntoBaseTypeWithTransformations(candidate->params.back()->ptype->toVariadicArrayType()->base);

			std::string baset;
			if(pt->isNamedType())
			{
				baset = pt->toNamedType()->name;
			}
			else
			{
				error("notsup");
			}


			if(thistm.find(baset) != thistm.end())
			{
				// already have it
				// ensure it matches with the ones we've already found.

				fir::Type* resolved = thistm[baset];
				iceAssert(resolved);

				if(args.size() >= candidate->params.size())
				{
					iceAssert(candidate->params.back()->ptype->isVariadicArrayType());
					pts::Type* vbase = candidate->params.back()->ptype->toVariadicArrayType()->base;

					pts::Type* _ = 0; std::deque<pts::TypeTransformer> trfs;
					std::tie(_, trfs) = pts::decomposeTypeIntoBaseTypeWithTransformations(vbase);
					fir::Type* resolvedvbase = pts::applyTransformationsOnType(resolved, trfs);


					if(args.size() == candidate->params.size() && args.back()->isParameterPackType())
					{
						// check transformations.
						fir::Type* fvbase = args.back()->toParameterPackType()->getElementType();

						fir::Type* _ = 0; std::deque<pts::TypeTransformer> argtrfs;
						std::tie(_, argtrfs) = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(fvbase);

						if(resolvedvbase != fvbase && cgi->getAutoCastDistance(fvbase, resolvedvbase) == -1)
						{
							// damn, this is a verbose error message.
							if(errorString && failedExpr)
							{
								*errorString = _makeErrorString("Incompatible base types for direct parameter pack-forwarding; no conversion from given base type '%s' to expected base type '%s' (in solution for parametric type '%s', solved as '%s')",
									fvbase->str().c_str(), resolvedvbase->str().c_str(), baset.c_str(), resolved->str().c_str());

								*failedExpr = candidate->params.back();
							}
							return false;
						}

						if(!pts::areTransformationsCompatible(trfs, argtrfs))
						{
							// damn, this is a verbose error message.
							if(errorString && failedExpr)
							{
								*errorString = _makeErrorString("Incompatible base types for direct parameter pack-forwarding; no transformations to get from given base type '%s' to parametric type '%s' with existing solution '%s' (ie. expected base type '%s')", fvbase->str().c_str(), baset.c_str(), resolved->str().c_str(), resolvedvbase->str().c_str());

								*failedExpr = candidate->params.back();
							}
							return false;
						}
					}
					else
					{
						for(size_t i = candidate->params.size() - 1; i < args.size(); i++)
						{
							if(cgi->getAutoCastDistance(args[i], resolvedvbase) == -1)
							{
								// damn, this is a verbose error message.
								if(errorString && failedExpr)
								{
									*errorString = _makeErrorString("Incompatible parameter in variadic argument to function; no conversion from given type '%s' to expected type '%s', in variadic argument %zu", args[i]->str().c_str(),
										resolvedvbase->str().c_str(), i + 2 - candidate->params.size());

									*failedExpr = candidate->params.back();
								}
								return false;
							}
						}
					}
				}
				else
				{
					// no varargs were even given
					// since we have already inferred the type from the other parameters,
					// we can give this a free pass.
				}
			}
			else if(candidate->genericTypes.find(baset) != candidate->genericTypes.end())
			{
				// ok, we need to be able to deduce the type from the vararg only.
				// so if none were provided, then give up.

				if(args.size() < candidate->params.size())
				{
					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("No variadic parameters were given, so no solution for parametric type '%s' could be determined (it is only used in the variadic parameter pack)", baset.c_str());

						*failedExpr = candidate->params.back();
					}
					return false;
				}


				// great, now just deduce it.
				// we just need to make sure all the Ts match, and the number of indirections
				// match *and* are greater than or equal to the specified level.

				fir::Type* first = args[candidate->params.size() - 1];

				// ok, check the type itself
				for(size_t i = candidate->params.size() - 1; i < args.size(); i++)
				{
					if(args[i] != first && cgi->getAutoCastDistance(first, args[i]))
					{
						if(errorString && failedExpr)
						{
							*errorString = _makeErrorString("Conflicting types in variadic arguments -- '%s' and '%s' (in solution for parametric type '%s')", args[i]->str().c_str(), first->str().c_str(), baset.c_str());

							*failedExpr = candidate->params.back();
						}
						return false;
					}

					// ok check the transformations
					fir::Type* base = 0; std::deque<pts::TypeTransformer> trfs;
					std::tie(base, trfs) = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(args[i]);

					if(!pts::areTransformationsCompatible(_trfs, trfs))
					{
						if(errorString && failedExpr)
						{
							*errorString = _makeErrorString("Incompatible base types for variadic argument %zu; no transformations to get from given base type '%s' to expected type '%s'  (in solution for parametric type '%s')", i + 1,
								args[i]->str().c_str(), candidate->params.back()->ptype->toDynamicArrayType()->base->str().c_str(),
								baset.c_str());

							*failedExpr = candidate->params.back();
						}
						return false;
					}
				}


				// everything checked out -- resolve.
				fir::Type* _ = 0; std::deque<pts::TypeTransformer> argtrfs;
				std::tie(_, argtrfs) = pts::decomposeFIRTypeIntoBaseTypeWithTransformations(first);
				fir::Type* soln = pts::reduceMaximallyWithSubset(first, _trfs, argtrfs);
				thistm[baset] = soln;
			}
		}




		// check that we actually have an entry for every type
		for(auto t : candidate->genericTypes)
		{
			if(thistm.find(t.first) == thistm.end())
			{
				if(genericPositions2.find(t.first) != genericPositions2.end())
				{
					for(auto i : genericPositions2[t.first])
					{
						// we have a chance
						// copy the existing solution
						std::map<std::string, fir::Type*> solns = thistm;
						if(candidate->params[i]->ptype->isFunctionType())
						{
							std::string es; Expr* fe = 0;
							bool res = checkGenericFunctionOrTupleArgument(cgi, candidate, i, candidate->params[i]->ptype,
								args[i], &solns, &es, &fe);

							if(!res)
							{
								*errorString = es;
								*failedExpr = fe;
								return false;
							}

							// update.
							thistm = solns;
						}
						else if(candidate->params[i]->ptype->isTupleType())
						{
							error("notsup");
						}
						else
						{
							iceAssert("??" && 0);
						}
					}
				}
				else
				{
					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("No solution for parametric type '%s' was found", t.first.c_str());
						*failedExpr = candidate;
					}
					return false;
				}
			}
		}



		// now that we did that, try again, making sure this time.
		for(auto t : candidate->genericTypes)
		{
			if(thistm.find(t.first) == thistm.end())
			{
				if(errorString && failedExpr)
				{
					*errorString = _makeErrorString("No solution for parametric type '%s' was found", t.first.c_str());
					*failedExpr = candidate;
				}
				return false;
			}
		}








		// last phase: ensure the type constraints are met
		for(auto cst : thistm)
		{
			TypeConstraints_t constr = candidate->genericTypes[cst.first];

			for(auto protstr : constr.protocols)
			{
				ProtocolDef* prot = cgi->resolveProtocolName(candidate, protstr);
				iceAssert(prot);

				bool doesConform = prot->checkTypeConformity(cgi, cst.second);

				if(!doesConform)
				{
					if(errorString && failedExpr)
					{
						*errorString = _makeErrorString("Solution for parametric type '%s' ('%s') does not conform to protocol '%s'",
							cst.first.c_str(), cst.second->str().c_str(), protstr.c_str());

						*failedExpr = candidate;
					}
					return false;
				}
			}
		}





		*gtm = thistm;
		return true;
	}











	FuncDefPair CodegenInstance::instantiateGenericFunctionUsingParameters(Expr* user, std::map<std::string, fir::Type*> _gtm,
		Func* func, std::deque<fir::Type*> params, std::string* err, Ast::Expr** ex)
	{
		iceAssert(func);
		iceAssert(func->decl);

		FuncDecl* fnDecl = func->decl;

		std::map<std::string, fir::Type*> gtm = _gtm;
		if(gtm.empty())
		{
			bool res = checkGenericFunction(this, &gtm, func->decl, params, err, ex);
			if(!res) return FuncDefPair::empty();
		}


		bool needToCodegen = true;
		if(this->reifiedGenericFunctions.find({ func, gtm }) != this->reifiedGenericFunctions.end())
			needToCodegen = false;




		// we need to push a new "generic type stack", and add the types that we resolved into it.
		// todo: might be inefficient.
		// todo: look into creating a version of pushGenericTypeStack that accepts a std::map<string, fir::Type*>
		// so we don't have to iterate etc etc.
		// I don't want to access cgi->instantiatedGenericTypeStack directly.



		fir::Function* ffunc = nullptr;
		if(needToCodegen)
		{
			Result_t res = fnDecl->generateDeclForGenericFunction(this, gtm);
			ffunc = (fir::Function*) res.value;

			this->reifiedGenericFunctions[{ func, gtm }] = ffunc;
		}
		else
		{
			ffunc = this->reifiedGenericFunctions[{ func, gtm }];
			iceAssert(ffunc);
		}

		iceAssert(ffunc);

		this->pushGenericTypeStack();
		for(auto pair : gtm)
			this->pushGenericType(pair.first, pair.second);

		if(needToCodegen)
		{
			// dirty: use 'lhsPtr' to pass the version we want.
			auto s = this->saveAndClearScope();
			func->codegen(this, ffunc);
			this->restoreScope(s);
		}

		this->removeFunctionFromScope(FuncDefPair(0, func->decl, func));
		this->popGenericTypeStack();

		return FuncDefPair(ffunc, func->decl, func);
	}


	FuncDefPair CodegenInstance::tryResolveGenericFunctionCallUsingCandidates(FuncCall* fc, std::deque<Func*> candidates,
		std::map<Func*, std::pair<std::string, Expr*>>* errs)
	{
		// try and resolve shit
		std::map<std::string, fir::Type*> gtm;

		if(candidates.size() == 0)
		{
			return FuncDefPair::empty();	// just fail
		}

		std::deque<fir::Type*> fargs;
		for(auto p : fc->params)
			fargs.push_back(p->getType(this));

		auto it = candidates.begin();
		while(it != candidates.end())
		{
			std::string s; Expr* e = 0;
			bool result = checkGenericFunction(this, &gtm, (*it)->decl, fargs, &s, &e);

			if(!result)
			{
				if(errs) (*errs)[*it] = { s, e };
				it = candidates.erase(it);
			}
			else
			{
				it++;
			}
		}

		if(candidates.size() == 0)
		{
			return FuncDefPair::empty();
		}
		else if(candidates.size() > 1)
		{
			std::string cands;
			for(auto c : candidates)
				cands += this->printAst(c->decl) + "\n";

			error(fc, "Ambiguous instantiation of parametric function %s, have %zd candidates:\n%s\n", fc->name.c_str(),
				candidates.size(), cands.c_str());
		}

		// we know gtm isn't empty, and we only set the errors if we need to verify
		// so we can safely ignore them here.
		std::string _; Expr* __ = 0;
		return this->instantiateGenericFunctionUsingParameters(fc, gtm, candidates[0], fargs, &_, &__);
	}

	FuncDefPair CodegenInstance::tryResolveGenericFunctionCall(FuncCall* fc, std::map<Func*, std::pair<std::string, Expr*>>* errs)
	{
		std::deque<Func*> candidates = this->findGenericFunctions(fc->name);
		return this->tryResolveGenericFunctionCallUsingCandidates(fc, candidates, errs);
	}


	FuncDefPair CodegenInstance::tryResolveGenericFunctionFromCandidatesUsingFunctionType(Expr* user, std::deque<Func*> candidates,
		fir::FunctionType* ft, std::map<Func*, std::pair<std::string, Expr*>>* errs)
	{
		std::deque<FuncDefPair> ret;
		for(auto fn : candidates)
		{
			std::string s; Expr* e = 0;
			auto fp = this->instantiateGenericFunctionUsingParameters(user, std::map<std::string, fir::Type*>(), fn,
				ft->getArgumentTypes(), &s, &e);

			if(fp.firFunc && fp.funcDef)
				ret.push_back(fp);

			else if(errs)
				(*errs)[fn] = { s, e };
		}

		if(ret.empty())
		{
			return FuncDefPair::empty();
		}
		else if(candidates.size() > 1)
		{
			std::string cands;
			for(auto c : candidates)
				cands += this->printAst(c->decl) + "\n";

			error(user, "Ambiguous instantiation of parametric function %s, have %zd candidates:\n%s\n",
				ret.front().funcDecl->ident.name.c_str(), candidates.size(), cands.c_str());
		}

		return ret.front();
	}





	fir::Function* CodegenInstance::instantiateGenericFunctionUsingValueAndType(Expr* user, fir::Function* oldfn, fir::FunctionType* oldft,
		fir::FunctionType* ft, MemberAccess* ma)
	{
		iceAssert(ft && ft->isFunctionType());
		if(ft->toFunctionType()->isGenericFunction())
		{
			error(user, "Unable to infer the instantiation of parametric function (type '%s'); explicit type specifier must be given",
				ft->str().c_str());
		}
		else
		{
			// concretised function is *not* generic.
			// hooray.

			fir::Function* fn = 0;
			std::map<Func*, std::pair<std::string, Expr*>> errs;
			if(ma)
			{
				auto fp = this->resolveAndInstantiateGenericFunctionReference(user, oldft, ft->toFunctionType(), ma, &errs);
				fn = fp;
			}
			else
			{
				if(!oldfn)
					error(user, "Could not resolve generic function??");

				auto fp = this->tryResolveGenericFunctionFromCandidatesUsingFunctionType(user,
					this->findGenericFunctions(oldfn->getName().name), ft->toFunctionType(), &errs);

				fn = fp.firFunc;
			}

			if(fn == 0)
			{
				exitless_error(user, "Invalid instantiation of parametric function of type '%s' with type '%s'",
					oldft->str().c_str(), ft->str().c_str());

				if(errs.size() > 0)
				{
					for(auto p : errs)
		 				info(p.first, "Candidate not suitable: %s", p.second.first.c_str());
				}

				doTheExit();
			}
			else
			{
				// return new history
				return fn;
			}
		}
	}





































	std::string CodegenInstance::mangleGenericParameters(std::deque<VarDecl*> args)
	{
		std::deque<std::string> strs;
		std::map<std::string, int> uniqueGenericTypes;	// only a map because it's easier to .find().

		// TODO: this is very suboptimal
		int runningTypeIndex = 0;
		for(auto arg : args)
		{
			fir::Type* atype = arg->getType(this, true);	// same as mangleFunctionName, but allow failures.

			// if there is no proper type, go ahead with the raw type: T or U or something.
			if(!atype)
			{
				std::string st = arg->ptype->str();
				if(uniqueGenericTypes.find(st) == uniqueGenericTypes.end())
				{
					uniqueGenericTypes[st] = runningTypeIndex;
					runningTypeIndex++;
				}
			}
		}

		// very very suboptimal.

		for(auto arg : args)
		{
			fir::Type* atype = arg->getType(this, true);	// same as mangleFunctionName, but allow failures.

			// if there is no proper type, go ahead with the raw type: T or U or something.
			if(!atype)
			{
				std::string st = arg->ptype->str();
				iceAssert(uniqueGenericTypes.find(st) != uniqueGenericTypes.end());

				std::string s = "GT" + std::to_string(uniqueGenericTypes[st]);
				strs.push_back(std::to_string(s.length()) + s);
			}
			else
			{
				std::string mangled = atype->encodedStr();

				if(atype->isParameterPackType())
				{
					mangled = "V" + atype->toParameterPackType()->encodedStr();
				}

				while(atype->isPointerType())
					mangled += "P", atype = atype->getPointerElementType();

				strs.push_back(mangled);
			}
		}

		std::string ret;
		for(auto s : strs)
			ret += "_" + s;

		return ret;
	}
}