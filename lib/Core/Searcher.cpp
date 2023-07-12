//===-- Searcher.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Searcher.h"
#include "TimingSolver.h"

#include "CoreStats.h"
#include "Executor.h"
#include "PTree.h"
#include "StatsTracker.h"

#include "klee/ExecutionState.h"
#include "klee/MergeHandler.h"
#include "klee/Statistics.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/ADT/DiscretePDF.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Support/ModuleUtil.h"
#include "klee/Internal/System/Time.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"

#include <cassert>
#include <fstream>
#include <climits>
#include <unordered_set>
#include <chrono>
#include <math.h>

using namespace klee;
using namespace llvm;
using namespace std::literals::chrono_literals;


namespace klee {
  extern RNG theRNG;
  // extern llvm::cl::opt<bool> IgnoreSolverFailures;
}

extern llvm::cl::opt<bool> IgnoreSolverFailures(
    "ignore-solver-failures",
    cl::init(true)
    );


cl::opt<std::string> MaxReviveTime(
    "max-revive-time",
    cl::desc("Maximum time to spend reviving states (default unlimited)")
    );

cl::opt<bool> RandomPendingDelition(
    "random-pending-deletion",
    cl::init(false),
    cl::desc("If enabled don't try to revive states for deleition (default=false)")
    );

cl::opt<int> ZestiBound(
    "zesti-bound-mul",
    cl::init(2),
    cl::desc("Bounds multiplier for zesti (default=2)")
    );



Searcher::~Searcher() {
}

SQLIntStatistic pendingRevives("PendingRevives", "PRev");
SQLIntStatistic pendingKills("PendingKills", "PKills");
SQLIntStatistic infeasibleConstraintsQueryTime("InfeasibleQueryTime", "iQT");
SQLIntStatistic infeasibleKillingConstraintsQueryTime("InfeasibleKillingQueryTime", "IkQT");
///

ExecutionState &DFSSearcher::selectState() {
  return *states.back();
}

void DFSSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  states.insert(states.end(),
                addedStates.begin(),
                addedStates.end());
  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    if (es == states.back()) {
      states.pop_back();
    } else {
      bool ok = false;

      for (std::vector<ExecutionState*>::iterator it = states.begin(),
             ie = states.end(); it != ie; ++it) {
        if (es==*it) {
          states.erase(it);
          ok = true;
          break;
        }
      }

      (void) ok;
//      assert(ok && "invalid state removed");
    }
  }
}

///

ExecutionState &BFSSearcher::selectState() {
  return *states.front();
}

void BFSSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  // Assumption: If new states were added KLEE forked, therefore states evolved.
  // constraints were added to the current state, it evolved.
  if (!addedStates.empty() && current &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end()) {
    auto pos = std::find(states.begin(), states.end(), current);
    assert(pos != states.end());
    states.erase(pos);
    states.push_back(current);
  }

  states.insert(states.end(),
                addedStates.begin(),
                addedStates.end());
  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    if (es == states.front()) {
      states.pop_front();
    } else {
      bool ok = false;

      for (std::deque<ExecutionState*>::iterator it = states.begin(),
             ie = states.end(); it != ie; ++it) {
        if (es==*it) {
          states.erase(it);
          ok = true;
          break;
        }
      }

      (void) ok;
      assert(ok && "invalid state removed");
    }
  }
}

///

ExecutionState &RandomSearcher::selectState() {
  return *states[theRNG.getInt32()%states.size()];
}

void
RandomSearcher::update(ExecutionState *current,
                       const std::vector<ExecutionState *> &addedStates,
                       const std::vector<ExecutionState *> &removedStates) {
  states.insert(states.end(),
                addedStates.begin(),
                addedStates.end());
  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    __attribute__((unused))
    bool ok = false;

    for (std::vector<ExecutionState*>::iterator it = states.begin(),
           ie = states.end(); it != ie; ++it) {
      if (es==*it) {
        states.erase(it);
        ok = true;
        break;
      }
    }
    
    assert(ok && "invalid state removed");
  }
}

///

WeightedRandomSearcher::WeightedRandomSearcher(WeightType _type)
  : states(new DiscretePDF<ExecutionState*>()),
    type(_type) {
  switch(type) {
  case Depth: 
  case RP: 
    updateWeights = false;
    break;
  case InstCount:
  case CPInstCount:
  case QueryCost:
  case MinDistToUncovered:
  case CoveringNew:
    updateWeights = true;
    break;
  default:
    assert(0 && "invalid weight type");
  }
}

WeightedRandomSearcher::~WeightedRandomSearcher() {
  delete states;
}

ExecutionState &WeightedRandomSearcher::selectState() {
  return *states->choose(theRNG.getDoubleL());
}

double WeightedRandomSearcher::getWeight(ExecutionState *es) {
  switch(type) {
  default:
  case Depth: 
    return es->depth;
  case RP: 
    return pow(2.0, -1*es->depth);
  case InstCount: {
    uint64_t count = theStatisticManager->getIndexedValue(stats::instructions,
                                                          es->pc->info->id);
    double inv = 1. / std::max((uint64_t) 1, count);
    return inv * inv;
  }
  case CPInstCount: {
    StackFrame &sf = es->stack.back();
    uint64_t count = sf.callPathNode->statistics.getValue(stats::instructions);
    double inv = 1. / std::max((uint64_t) 1, count);
    return inv;
  }
  case QueryCost:
    return (es->queryCost.toSeconds() < .1) ? 1. : 1./ es->queryCost.toSeconds();
  case CoveringNew:
  case MinDistToUncovered: {
    uint64_t md2u = computeMinDistToUncovered(es->pc,
                                              es->stack.back().minDistToUncoveredOnReturn);

    double invMD2U = 1. / (md2u ? md2u : 10000);
    if (type==CoveringNew) {
      double invCovNew = 0.;
      if (es->instsSinceCovNew)
        invCovNew = 1. / std::max(1, (int) es->instsSinceCovNew - 1000);
      return (invCovNew * invCovNew + invMD2U * invMD2U);
    } else {
      return invMD2U * invMD2U;
    }
  }
  }
}

void WeightedRandomSearcher::update(
    ExecutionState *current, const std::vector<ExecutionState *> &addedStates,
    const std::vector<ExecutionState *> &removedStates) {
  size = size + addedStates.size() - removedStates.size();
  if (current && updateWeights &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end())
    states->update(current, getWeight(current));

  for (std::vector<ExecutionState *>::const_iterator it = addedStates.begin(),
                                                     ie = addedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    states->insert(es, getWeight(es));
  }

  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    states->remove(*it);
  }
}

bool WeightedRandomSearcher::empty() { 
  return states->empty(); 
}

int RandomPathSearcher::numRPSearchers = 0;

///
RandomPathSearcher::RandomPathSearcher(Executor &_executor)
  : executor(_executor), idBitMask(1 << numRPSearchers) {
      assert(numRPSearchers < 3 && "Too many RandomPath searcher created (pointer bit limit)");
      numRPSearchers++;
}

RandomPathSearcher::~RandomPathSearcher() {
}

#define IS_OUR_NODE_VALID(n) ((((n).getInt() & idBitMask) != 0) && ((n).getPointer() != nullptr))
ExecutionState &RandomPathSearcher::selectState() {
  unsigned flips=0, bits=0;
  assert(executor.processTree->root.getInt() & idBitMask && "Root should belong to the searcher");
  PTree::Node *n = executor.processTree->root.getPointer();
  while (!n->data) {
    if (!IS_OUR_NODE_VALID(n->left)) {
      if(!IS_OUR_NODE_VALID(n->right)) 
          errs() << "n parent " << n->parent << "\n";
      assert(IS_OUR_NODE_VALID(n->right) && "Both left and right nodes invalid");
      assert(n != n->right.getPointer());
      n = n->right.getPointer();
    } else if (!IS_OUR_NODE_VALID(n->right)) {
      assert(IS_OUR_NODE_VALID(n->left) && "Both right and left nodes invalid");
      assert(n != n->left.getPointer());
      n = n->left.getPointer();
    } else {
      if (bits==0) {
        flips = theRNG.getInt32();
        bits = 32;
      }
      --bits;
      n = ((flips&(1<<bits)) ? n->left : n->right).getPointer();
    }
  }
  return *n->data;
}

std::vector<ExecutionState *> RandomPathSearcher::selectForDelition(int size) {
    errs() << "RP Deliting " << size << " states\n";
    std::vector<ExecutionState* > ret;
    ret.reserve(size);
    for(auto& es : executor.states) {
        size--;
        if(size < 1) break;
        ret.push_back(es);
    }
    return ret;
}

std::vector<ExecutionState *> WeightedRandomSearcher::selectForDelition(int size) {
    std::unordered_set<ExecutionState*> ret;
    while(size > 0) {
        size--;
        ret.insert(&selectState());
    }
    std::vector<ExecutionState* > ret1(ret.begin(), ret.end());
    errs() << "NURs Deliting " << ret1.size() << " states\n";
    return ret1;
}
void
RandomPathSearcher::update(ExecutionState *current,
                           const std::vector<ExecutionState *> &addedStates,
                           const std::vector<ExecutionState *> &removedStates) {
  size = size + addedStates.size() - removedStates.size();
  for (auto es : addedStates) {
    PTreeNode *pnode = es->ptreeNode, *parent = pnode->parent;
    PTreeNodePtr *childPtr;

    childPtr = parent ? ((parent->left.getPointer() == pnode) ? &parent->left
                                                              : &parent->right)
                      : &executor.processTree->root;
    while (pnode && !IS_OUR_NODE_VALID(*childPtr)) {
      childPtr->setInt(childPtr->getInt() | idBitMask);
      pnode = parent;
      if (pnode)
        parent = pnode->parent;

      childPtr = parent
                     ? ((parent->left.getPointer() == pnode) ? &parent->left
                                                             : &parent->right)
                     : &executor.processTree->root;
    }
  }

	for (auto es : removedStates) {
    PTreeNode *pnode = es->ptreeNode, *parent = pnode->parent;

    while (pnode && !IS_OUR_NODE_VALID(pnode->left) &&
           !IS_OUR_NODE_VALID(pnode->right)) {
      auto childPtr =
          parent ? ((parent->left.getPointer() == pnode) ? &parent->left
                                                         : &parent->right)
                 : &executor.processTree->root;
      assert(IS_OUR_NODE_VALID(*childPtr) && "Removing pTree child not ours");
      childPtr->setInt(childPtr->getInt() & ~idBitMask);
      pnode = parent;
      if (pnode)
        parent = pnode->parent;
    }
  }  

}

bool RandomPathSearcher::empty() {  
	return !IS_OUR_NODE_VALID(executor.processTree->root);
}

///

MergingSearcher::MergingSearcher(Executor &_executor, Searcher *_baseSearcher)
  : executor(_executor),
  baseSearcher(_baseSearcher){}

MergingSearcher::~MergingSearcher() {
  delete baseSearcher;
}

ExecutionState& MergingSearcher::selectState() {
  assert(!baseSearcher->empty() && "base searcher is empty");

  // Iterate through all MergeHandlers
  for (auto cur_mergehandler: executor.mergeGroups) {
    // Find one that has states that could be released
    if (!cur_mergehandler->hasMergedStates()) {
      continue;
    }
    // Find a state that can be prioritized
    ExecutionState *es = cur_mergehandler->getPrioritizeState();
    if (es) {
      return *es;
    } else {
      if (DebugLogIncompleteMerge){
        llvm::errs() << "Preemptively releasing states\n";
      }
      // If no state can be prioritized, they all exceeded the amount of time we
      // are willing to wait for them. Release the states that already arrived at close_merge.
      cur_mergehandler->releaseStates();
    }
  }
  // If we were not able to prioritize a merging state, just return some state
  return baseSearcher->selectState();
}

///
PendingSearcher::PendingSearcher(Searcher *_baseNormalSearcher, Searcher* _basePendingSearcher,
                                   Executor* _exec) 
  : baseNormalSearcher(_baseNormalSearcher), basePendingSearcher(_basePendingSearcher), exec(_exec) {
      maxReviveTime = time::Span(MaxReviveTime);
}

PendingSearcher::~PendingSearcher() {
  delete baseNormalSearcher;
  delete basePendingSearcher;
}


std::vector<ExecutionState *> PendingSearcher::selectForDelition(int size) {
    if(RandomPendingDelition) { 
        auto ret = basePendingSearcher->selectForDelition(size);
        if(size - ret.size() > 0) {
            auto normalDeletes = baseNormalSearcher->selectForDelition(size - ret.size());
            ret.insert(ret.end(), normalDeletes.begin(), normalDeletes.end());
        }
        return ret;

    }
    errs() << "Deliting " << size << " states\n";
    int revived = 0, killed = 0;
 
    exec->solver->setTimeout(maxReviveTime);
    bool rememberIgnore =  IgnoreSolverFailures;
    IgnoreSolverFailures = true;
    while(!basePendingSearcher->empty() && size - revived > 0 && (basePendingSearcher->getSize() > size)) {
      if(exec->haltExecution) return {};
      auto& es = basePendingSearcher->selectState();
      assert(!es.pendingConstraint.isNull());
      ref<Expr> expr = es.pendingConstraint;
      es.pendingConstraint = nullptr;
      assert(expr->getWidth());
      bool status = false, solverResult = false;
      TimerStatIncrementer t(infeasibleKillingConstraintsQueryTime);
      status = exec->solver->mayBeTrue(es, expr, solverResult);
      if(status && solverResult) {
          exec->addConstraint(es, expr);
          baseNormalSearcher->update(nullptr, {&es}, {});
          basePendingSearcher->update(nullptr,{}, {&es});
          revived++;
          ++pendingRevives;
          t.ignore();
 //         llvm::errs() << "success\n";
      } else {
 //         llvm::errs() << "killing it\n";
          killed++;
          ++pendingKills;
          size--;
          basePendingSearcher->update(nullptr,{}, {&es});
          exec->processTree->remove(es.ptreeNode);
          auto it2 = exec->states.find(&es);
          assert(it2!=exec->states.end());
          exec->states.erase(it2);
          delete &es;
      }

    }
    exec->solver->setTimeout(time::Span());
    IgnoreSolverFailures = rememberIgnore;
    errs() << "==== Deleted " << killed << " and revived " << revived << "\n";

    //assert(!basePendingSearcher->empty() && "TODO delete more states than are pending");
  //  if(basePendingSearcher->empty()) {
  //      klee_warning("Deleted as many pending states as possible");
  //  }
    return baseNormalSearcher->selectForDelition(size);
}

bool PendingSearcher::empty() { 
  if(!baseNormalSearcher->empty()) return false;


  bool solverResult = false, status = false;
  exec->solver->setTimeout(maxReviveTime);
  while(baseNormalSearcher->empty()) {
      if(basePendingSearcher->empty()) return true;
//      llvm::errs() << "Reviving pending state: ";
      auto& es = basePendingSearcher->selectState();
      assert(!es.pendingConstraint.isNull());
      ref<Expr> expr = es.pendingConstraint;
      es.pendingConstraint = nullptr;
      assert(expr->getWidth());
      TimerStatIncrementer t(infeasibleConstraintsQueryTime);
      status = exec->solver->mayBeTrue(es, expr, solverResult);
      if(status && solverResult) {
          exec->addConstraint(es, expr);
          baseNormalSearcher->update(nullptr, {&es}, {});
          basePendingSearcher->update(nullptr,{}, {&es});
//          llvm::errs() << "success\n";
          t.ignore();
          ++pendingRevives;
      } else {
//          llvm::errs() << "killing it\n";
          ++pendingKills;
          basePendingSearcher->update(nullptr,{}, {&es});
          exec->processTree->remove(es.ptreeNode);
          auto it2 = exec->states.find(&es);
          assert(it2!=exec->states.end());
          exec->states.erase(it2);
          delete &es;
      }
  }
  exec->solver->setTimeout(time::Span());

  return baseNormalSearcher->empty() && basePendingSearcher->empty(); 
}

ExecutionState &PendingSearcher::selectState() {
  return baseNormalSearcher->selectState();
}

void
PendingSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  if(addedStates.size() > 0 || removedStates.size() > 0) {
  //    errs() << "Adding " << addedStates.size() << " Removing " << removedStates.size() << "\n"; 
  }

  auto is_pending = [](const auto& es) {return !es->pendingConstraint.isNull(); }; 
  std::vector<ExecutionState* > addedN, addedP, removedN, removedP;

  for(const auto& es : addedStates) {
      if(is_pending(es)) 
          addedP.push_back(es);
      else
          addedN.push_back(es);
  }

  for(const auto& es : removedStates) {
      if(is_pending(es)) 
          removedP.push_back(es);
      else
          removedN.push_back(es);
  }
  
  if (current && is_pending(current)) {
      removedN.push_back(current);
      addedP.push_back(current);
//      current = nullptr;
//      errs() << "Current nulled\n";
  }
  
  baseNormalSearcher->update(current, addedN, removedN);
  basePendingSearcher->update(nullptr, addedP, removedP);
  //if(addedStates.size() > 0 || removedStates.size() > 0) {
  //  llvm::errs() << "=Normal: " ;
  //  baseNormalSearcher->printName(errs());
  //  llvm::errs() << " Pending: " ;
  //  basePendingSearcher->printName(errs());
  //  llvm::errs() << "\n" ;
  //}

}

bool ZESTIPendingSearcher::empty() { 
  if(!hasSelectedState) {
      computeDistances();
  }
  hasSelectedState = true;
  //If zesti bound is 0, we do no exploration of pending states (ie. simple zesti)
  if(ZestiBound == 0) {
    return true;
  }
  auto pendingIt = pendingStates.rbegin();
  while(normalStates.empty()  && !pendingStates.empty()) {
  //while(normalStates.empty()  && pendingIt != pendingStates.rend()) {

      llvm::errs() << exec->senstiveDepths.size() <<  " Sensitive instructions at depths: [";
      for(const auto& dpth : exec->senstiveDepths) {
          llvm::errs() << dpth << ", ";
      }
      llvm::errs() << "\b\b]\n";

      llvm::errs() << "Pending ordering depths: [";
      for(const auto& pEs : pendingStates) {
          llvm::errs() << pEs->depth << ", ";
      }
      llvm::errs() << "\b\b]\n";


      auto es = pendingStates.back(); pendingStates.pop_back();
//      auto es  = *pendingIt;
      if(smallestSensitiveDistance[es] != 999999 && exec->attemptToRevive(es, exec->solver->solver)) {
          currentBaseDepth = es->depth;
          bound =  ZestiBound * smallestSensitiveDistance[es];
					bound = bound == 0 ? 1 : bound;
          update(nullptr, {es}, {});
          llvm::errs() << "ZESTI revive state at depth " << es->depth <<  " adding bound " << bound << "\n";
      } else {
          llvm::errs() << "ZESTI kill state at depth " << es->depth << "\n";

          exec->terminateState(*es);
          //kill it;
//          exec->processTree->remove(es->ptreeNode);
//          auto it2 = exec->states.find(es);
//          assert(it2!=exec->states.end());
//          exec->states.erase(it2);
//          delete es;
      }
      pendingIt++;
  }
  return normalSearcher->empty();

}

ZESTIPendingSearcher::ZESTIPendingSearcher(Executor* _exec) : exec(_exec) {
    normalSearcher = new DFSSearcher();
    bound = 0;

}

ExecutionState &ZESTIPendingSearcher::selectState() {
  if(!hasSelectedState) {
      computeDistances();
  }
  hasSelectedState = true;
  for(auto& es : toDelete) {
      exec->terminateState(*es);
  }
  toDelete.clear();

  return normalSearcher->selectState();
}

void ZESTIPendingSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  auto is_pending = [](const auto& es) {return !es->pendingConstraint.isNull(); }; 
  std::vector<ExecutionState* > addedN, addedP, removedN, removedP;

  for(const auto& es : addedStates) {
      if(is_pending(es)) 
          addedP.push_back(es);
      else if(currentBaseDepth >= 0 && (int)es->depth > currentBaseDepth + bound) {
          //This states is out of bounds of current exploration, therefore we termiante it
//          llvm::errs() << "Terminating states due to its depth: " << es->depth << " exceeding base depth " << currentBaseDepth << " + " << bound << "\n";
          toDelete.push_back(es);
      }
      else
          addedN.push_back(es);
  }

  for(const auto& es : removedStates) {
      if(is_pending(es)) 
          removedP.push_back(es);
      else
          removedN.push_back(es);
  }
  
  if (current && is_pending(current)) {
      removedN.push_back(current);
      addedP.push_back(current);
  } else if(current && currentBaseDepth >= 0 && (int)current->depth > currentBaseDepth + bound) {
          //This states is out of bounds of current exploration, therefore we termiante it
 //         llvm::errs() << "Terminating current state due to its depth: " << current->depth << " exceeding base depth " << currentBaseDepth << " + " << bound << "\n";
       bool currentIsRemoved = false;
       for(const auto& es : removedStates) {
           currentIsRemoved |= es == current;
       }

       if(!currentIsRemoved) toDelete.push_back(current);
       current = nullptr;
   }

  if(hasSelectedState) {
      assert(addedP.empty() && "ZESTI searcher assumes pending states has been disabled when it is active");
  }
  normalSearcher->update(current, addedN, removedN);

  normalStates.insert(normalStates.end(), addedN.begin(), addedN.end());
  pendingStates.insert(pendingStates.end(), addedP.begin(), addedP.end());

  for(const auto& es : removedN) {
      for (auto it = normalStates.begin(), ie = normalStates.end(); it != ie; ++it) {
        if (es==*it) 
          normalStates.erase(it);
      }
  }

  for(const auto& es : removedP) {
      for (auto it = pendingStates.begin(), ie = pendingStates.end(); it != ie; ++it) {
        if (es==*it) 
           pendingStates.erase(it);
      }
  }
//  llvm::errs() << "Normal states: " << normalStates.size() << " pending states: " << pendingStates.size() << "\r";
}

void ZESTIPendingSearcher::computeDistances() {
    //TODO only execute this once when switching
    //TODO just kill the state without a smallestDistance (999999)
    llvm::errs() << "Computing distance " << this << "\n";
    for(const auto& pEs : pendingStates) {
        int smallestDistance = 999999;
        for(const auto& sDepth : exec->senstiveDepths) {
            int diff = sDepth - pEs->depth;
            smallestDistance = diff >= 0 && diff < smallestDistance ? diff : smallestDistance;
        }
        smallestSensitiveDistance[pEs] = smallestDistance;
        if(smallestDistance == 999999) {
//            exec->terminateState(*pEs);
        }
    }
    std::sort(pendingStates.begin(), pendingStates.end(),
      [&](const ExecutionState* es1, const ExecutionState* es2) {
          if(smallestSensitiveDistance[es1] == smallestSensitiveDistance[es2])
            return es1->depth < es2->depth;
          else
            return (smallestSensitiveDistance[es1] > smallestSensitiveDistance[es2]);
      }
    );
}

BatchingSearcher::BatchingSearcher(Searcher *_baseSearcher,
                                   time::Span _timeBudget,
                                   unsigned _instructionBudget) 
  : baseSearcher(_baseSearcher),
    timeBudget(_timeBudget),
    instructionBudget(_instructionBudget),
    lastState(0) {
  
}

BatchingSearcher::~BatchingSearcher() {
  delete baseSearcher;
}

ExecutionState &BatchingSearcher::selectState() {
  if (!lastState ||
      (((timeBudget.toSeconds() > 0) &&
        (time::getWallTime() - lastStartTime) > timeBudget)) ||
      ((instructionBudget > 0) &&
       (stats::instructions - lastStartInstructions) > instructionBudget)) {
    if (lastState) {
      time::Span delta = time::getWallTime() - lastStartTime;
      auto t = timeBudget;
      t *= 1.1;
      if (delta > t) {
        klee_message("increased time budget from %f to %f\n", timeBudget.toSeconds(), delta.toSeconds());
        timeBudget = delta;
      }
    }
    lastState = &baseSearcher->selectState();
    lastStartTime = time::getWallTime();
    lastStartInstructions = stats::instructions;
    return *lastState;
  } else {
    return *lastState;
  }
}

void
BatchingSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  if (std::find(removedStates.begin(), removedStates.end(), lastState) !=
      removedStates.end())
    lastState = 0;
  baseSearcher->update(current, addedStates, removedStates);
}

/***/

IterativeDeepeningTimeSearcher::IterativeDeepeningTimeSearcher(Searcher *_baseSearcher)
  : baseSearcher(_baseSearcher),
    time(time::seconds(1)) {
}

IterativeDeepeningTimeSearcher::~IterativeDeepeningTimeSearcher() {
  delete baseSearcher;
}

ExecutionState &IterativeDeepeningTimeSearcher::selectState() {
  ExecutionState &res = baseSearcher->selectState();
  startTime = time::getWallTime();
  return res;
}

void IterativeDeepeningTimeSearcher::update(
    ExecutionState *current, const std::vector<ExecutionState *> &addedStates,
    const std::vector<ExecutionState *> &removedStates) {

  const auto elapsed = time::getWallTime() - startTime;

  if (!removedStates.empty()) {
    std::vector<ExecutionState *> alt = removedStates;
    for (std::vector<ExecutionState *>::const_iterator
             it = removedStates.begin(),
             ie = removedStates.end();
         it != ie; ++it) {
      ExecutionState *es = *it;
      std::set<ExecutionState*>::const_iterator it2 = pausedStates.find(es);
      if (it2 != pausedStates.end()) {
        pausedStates.erase(it2);
        alt.erase(std::remove(alt.begin(), alt.end(), es), alt.end());
      }
    }    
    baseSearcher->update(current, addedStates, alt);
  } else {
    baseSearcher->update(current, addedStates, removedStates);
  }

  if (current &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end() &&
      elapsed > time) {
    pausedStates.insert(current);
    baseSearcher->removeState(current);
  }

  if (baseSearcher->empty()) {
    time *= 2U;
    klee_message("increased time budget to %f\n", time.toSeconds());
    std::vector<ExecutionState *> ps(pausedStates.begin(), pausedStates.end());
    baseSearcher->update(0, ps, std::vector<ExecutionState *>());
    pausedStates.clear();
  }
}

/***/

InterleavedSearcher::InterleavedSearcher(const std::vector<Searcher*> &_searchers)
  : searchers(_searchers),
    index(1) {
}

InterleavedSearcher::~InterleavedSearcher() {
  for (std::vector<Searcher*>::const_iterator it = searchers.begin(),
         ie = searchers.end(); it != ie; ++it)
    delete *it;
}

ExecutionState &InterleavedSearcher::selectState() {
  Searcher *s = searchers[--index];
  if (index==0) index = searchers.size();
  return s->selectState();
}

void InterleavedSearcher::update(
    ExecutionState *current, const std::vector<ExecutionState *> &addedStates,
    const std::vector<ExecutionState *> &removedStates) {
  for (std::vector<Searcher*>::const_iterator it = searchers.begin(),
         ie = searchers.end(); it != ie; ++it)
    (*it)->update(current, addedStates, removedStates);
}