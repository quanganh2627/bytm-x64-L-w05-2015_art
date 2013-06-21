/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "file_output_stream.h"
#include "instruction_tools.h"
#include "sea.h"
#include "code_gen.h"

#define MAX_REACHING_DEF_ITERERATIONS (10)
// TODO: When development is done, this define should not
// be needed, it is currently used as a cutoff
// for cases where the iterative fixed point algorithm
// does not reach a fixed point because of a bug.

namespace sea_ir {

SeaGraph SeaGraph::graph_;
int SeaNode::current_max_node_id_ = 0;

void IRVisitor::Traverse(Region* region) {
  std::vector<PhiInstructionNode*>* phis = region->GetPhiNodes();
  for (std::vector<PhiInstructionNode*>::const_iterator cit = phis->begin();
      cit != phis->end(); cit++) {
    (*cit)->Accept(this);
  }

  std::vector<InstructionNode*>* instructions = region->GetInstructions();
  for (std::vector<InstructionNode*>::const_iterator cit = instructions->begin();
      cit != instructions->end(); cit++) {
    (*cit)->Accept(this);
  }
}

void IRVisitor::Traverse(SeaGraph* graph) {
  for (std::vector<Region*>::const_iterator cit = ordered_regions_.begin();
          cit != ordered_regions_.end(); cit++ ) {
    (*cit)->Accept(this);
  }
}

SeaGraph* SeaGraph::GetCurrentGraph() {
  return &sea_ir::SeaGraph::graph_;
}

void SeaGraph::DumpSea(std::string filename) const {
  LOG(INFO) << "Starting to write SEA string to file.";
  std::string result;
  result += "digraph seaOfNodes {\n";
  for (std::vector<Region*>::const_iterator cit = regions_.begin(); cit != regions_.end(); cit++) {
    (*cit)->ToDot(result);
  }
  result += "}\n";
  art::File* file = art::OS::OpenFile(filename.c_str(), true, true);
  art::FileOutputStream fos(file);
  fos.WriteFully(result.c_str(), result.size());
  LOG(INFO) << "Written SEA string to file.";
}

void SeaGraph::AddEdge(Region* src, Region* dst) const {
  src->AddSuccessor(dst);
  dst->AddPredecessor(src);
}

void SeaGraph::ComputeRPO(Region* current_region, int& current_rpo) {
  current_region->SetRPO(VISITING);
  std::vector<sea_ir::Region*>* succs = current_region->GetSuccessors();
  for (std::vector<sea_ir::Region*>::iterator succ_it = succs->begin();
      succ_it != succs->end(); ++succ_it) {
    if (NOT_VISITED == (*succ_it)->GetRPO()) {
      SeaGraph::ComputeRPO(*succ_it, current_rpo);
    }
  }
  current_region->SetRPO(current_rpo--);
}

void SeaGraph::ComputeIDominators() {
  bool changed = true;
  while (changed) {
    changed = false;
    // Entry node has itself as IDOM.
    std::vector<Region*>::iterator crt_it;
    std::set<Region*> processedNodes;
    // Find and mark the entry node(s).
    for (crt_it = regions_.begin(); crt_it != regions_.end(); ++crt_it) {
      if ((*crt_it)->GetPredecessors()->size() == 0) {
        processedNodes.insert(*crt_it);
        (*crt_it)->SetIDominator(*crt_it);
      }
    }
    for (crt_it = regions_.begin(); crt_it != regions_.end(); ++crt_it) {
      if ((*crt_it)->GetPredecessors()->size() == 0) {
        continue;
      }
      // NewIDom = first (processed) predecessor of b.
      Region* new_dom = NULL;
      std::vector<Region*>* preds = (*crt_it)->GetPredecessors();
      DCHECK(NULL != preds);
      Region* root_pred = NULL;
      for (std::vector<Region*>::iterator pred_it = preds->begin();
          pred_it != preds->end(); ++pred_it) {
        if (processedNodes.end() != processedNodes.find((*pred_it))) {
          root_pred = *pred_it;
          new_dom = root_pred;
          break;
        }
      }
      // For all other predecessors p of b, if idom is not set,
      // then NewIdom = Intersect(p, NewIdom)
      for (std::vector<Region*>::const_iterator pred_it = preds->begin();
          pred_it != preds->end(); ++pred_it) {
        DCHECK(NULL != *pred_it);
        // if IDOMS[p] != UNDEFINED
        if ((*pred_it != root_pred) && (*pred_it)->GetIDominator() != NULL) {
          DCHECK(NULL != new_dom);
          new_dom = SeaGraph::Intersect(*pred_it, new_dom);
        }
      }
      DCHECK(NULL != *crt_it);
      if ((*crt_it)->GetIDominator() != new_dom) {
        (*crt_it)->SetIDominator(new_dom);
        changed = true;
      }
      processedNodes.insert(*crt_it);
    }
  }

  // For easily ordering of regions we need edges dominator->dominated.
  for (std::vector<Region*>::iterator region_it = regions_.begin();
      region_it != regions_.end(); region_it++) {
    Region* idom = (*region_it)->GetIDominator();
    if (idom != *region_it) {
      idom->AddToIDominatedSet(*region_it);
    }
  }
}

Region* SeaGraph::Intersect(Region* i, Region* j) {
  Region* finger1 = i;
  Region* finger2 = j;
  while (finger1 != finger2) {
    while (finger1->GetRPO() > finger2->GetRPO()) {
      DCHECK(NULL != finger1);
      finger1 = finger1->GetIDominator(); // should have: finger1 != NULL
      DCHECK(NULL != finger1);
    }
    while (finger1->GetRPO() < finger2->GetRPO()) {
      DCHECK(NULL != finger2);
      finger2 = finger2->GetIDominator(); // should have: finger1 != NULL
      DCHECK(NULL != finger2);
    }
  }
  return finger1; // finger1 should be equal to finger2 at this point.
}

void SeaGraph::ComputeDownExposedDefs() {
  for (std::vector<Region*>::iterator region_it = regions_.begin();
        region_it != regions_.end(); region_it++) {
      (*region_it)->ComputeDownExposedDefs();
    }
}

void SeaGraph::ComputeReachingDefs() {
  // Iterate until the reaching definitions set doesn't change anymore.
  // (See Cooper & Torczon, "Engineering a Compiler", second edition, page 487)
  bool changed = true;
  int iteration = 0;
  while (changed && (iteration < MAX_REACHING_DEF_ITERERATIONS)) {
    iteration++;
    changed = false;
    // TODO: optimize the ordering if this becomes performance bottleneck.
    for (std::vector<Region*>::iterator regions_it = regions_.begin();
        regions_it != regions_.end();
        regions_it++) {
      changed |= (*regions_it)->UpdateReachingDefs();
    }
  }
  DCHECK(!changed) << "Reaching definitions computation did not reach a fixed point.";
}


void SeaGraph::BuildMethodSeaGraph(const art::DexFile::CodeItem* code_item,
    const art::DexFile& dex_file, uint32_t class_def_idx, uint32_t method_idx) {
  class_def_idx_ = class_def_idx;
  method_idx_ = method_idx;

  const uint16_t* code = code_item->insns_;
  const size_t size_in_code_units = code_item->insns_size_in_code_units_;
  // This maps target instruction pointers to their corresponding region objects.
  std::map<const uint16_t*, Region*> target_regions;
  size_t i = 0;
  // Pass: Find the start instruction of basic blocks
  //         by locating targets and flow-though instructions of branches.
  while (i < size_in_code_units) {
    const art::Instruction* inst = art::Instruction::At(&code[i]);
    if (inst->IsBranch() || inst->IsUnconditional()) {
      int32_t offset = inst->GetTargetOffset();
      if (target_regions.end() == target_regions.find(&code[i + offset])) {
        Region* region = GetNewRegion();
        target_regions.insert(std::pair<const uint16_t*, Region*>(&code[i + offset], region));
      }
      if (inst->CanFlowThrough()
          && (target_regions.end() == target_regions.find(&code[i + inst->SizeInCodeUnits()]))) {
        Region* region = GetNewRegion();
        target_regions.insert(
            std::pair<const uint16_t*, Region*>(&code[i + inst->SizeInCodeUnits()], region));
      }
    }
    i += inst->SizeInCodeUnits();
  }


  Region* r = GetNewRegion();
  // Insert one SignatureNode per function argument,
  // to serve as placeholder definitions in dataflow analysis.
  for (unsigned int crt_offset = 0; crt_offset < code_item->ins_size_; crt_offset++) {
    SignatureNode* parameter_def_node =
        new sea_ir::SignatureNode(code_item->registers_size_ - 1 - crt_offset);
    AddParameterNode(parameter_def_node);
    r->AddChild(parameter_def_node);
  }
  // Pass: Assign instructions to region nodes and
  //         assign branches their control flow successors.
  i = 0;
  sea_ir::InstructionNode* last_node = NULL;
  sea_ir::InstructionNode* node = NULL;
  while (i < size_in_code_units) {
    const art::Instruction* inst = art::Instruction::At(&code[i]);
    std::vector<InstructionNode*> sea_instructions_for_dalvik = sea_ir::InstructionNode::Create(inst);
    for (std::vector<InstructionNode*>::const_iterator cit = sea_instructions_for_dalvik.begin();
        sea_instructions_for_dalvik.end() != cit; ++cit) {
      last_node = node;
      node = *cit;

      if (inst->IsBranch() || inst->IsUnconditional()) {
        int32_t offset = inst->GetTargetOffset();
        std::map<const uint16_t*, Region*>::iterator it = target_regions.find(&code[i + offset]);
        DCHECK(it != target_regions.end());
        AddEdge(r, it->second); // Add edge to branch target.
      }

      std::map<const uint16_t*, Region*>::iterator it = target_regions.find(&code[i]);
      if (target_regions.end() != it) {
        // Get the already created region because this is a branch target.
        Region* nextRegion = it->second;
        if (last_node->GetInstruction()->IsBranch()
            && last_node->GetInstruction()->CanFlowThrough()) {
          AddEdge(r, it->second); // Add flow-through edge.
        }
        r = nextRegion;
      }
      bool definesRegister = (0 != InstructionTools::instruction_attributes_[inst->Opcode()]
          && (1 << kDA));
      LOG(INFO)<< inst->GetDexPc(code) << "*** " << inst->DumpString(&dex_file)
      << " region:" <<r->StringId() << "Definition?" << definesRegister << std::endl;
      r->AddChild(node);
      }
    i += inst->SizeInCodeUnits();
  }
}

void SeaGraph::ComputeRPO() {
  int rpo_id = regions_.size() - 1;
  for (std::vector<Region*>::const_iterator crt_it = regions_.begin(); crt_it != regions_.end();
      ++crt_it) {
    if ((*crt_it)->GetPredecessors()->size() == 0) {
      ComputeRPO(*crt_it, rpo_id);
    }
  }
}

// Performs the renaming phase in traditional SSA transformations.
// See: Cooper & Torczon, "Engineering a Compiler", second edition, page 505.)
void SeaGraph::RenameAsSSA() {
  utils::ScopedHashtable<int, InstructionNode*> scoped_table;
  scoped_table.OpenScope();
  for (std::vector<Region*>::iterator region_it = regions_.begin(); region_it != regions_.end();
      region_it++) {
    if ((*region_it)->GetIDominator() == *region_it) {
      RenameAsSSA(*region_it, &scoped_table);
    }
  }
  scoped_table.CloseScope();
}

void SeaGraph::ConvertToSSA() {
  // Pass: find global names.
  // The map @block maps registers to the blocks in which they are defined.
  std::map<int, std::set<Region*> > blocks;
  // The set @globals records registers whose use
  // is in a different block than the corresponding definition.
  std::set<int> globals;
  for (std::vector<Region*>::iterator region_it = regions_.begin(); region_it != regions_.end();
      region_it++) {
    std::set<int> var_kill;
    std::vector<InstructionNode*>* instructions = (*region_it)->GetInstructions();
    for (std::vector<InstructionNode*>::iterator inst_it = instructions->begin();
        inst_it != instructions->end(); inst_it++) {
      std::vector<int> used_regs = (*inst_it)->GetUses();
      for (std::size_t i = 0; i < used_regs.size(); i++) {
        int used_reg = used_regs[i];
        if (var_kill.find(used_reg) == var_kill.end()) {
          globals.insert(used_reg);
        }
      }
      const int reg_def = (*inst_it)->GetResultRegister();
      if (reg_def != NO_REGISTER) {
        var_kill.insert(reg_def);
      }

      blocks.insert(std::pair<int, std::set<Region*> >(reg_def, std::set<Region*>()));
      std::set<Region*>* reg_def_blocks = &(blocks.find(reg_def)->second);
      reg_def_blocks->insert(*region_it);
    }
  }

  // Pass: Actually add phi-nodes to regions.
  for (std::set<int>::const_iterator globals_it = globals.begin();
      globals_it != globals.end(); globals_it++) {
    int global = *globals_it;
    // Copy the set, because we will modify the worklist as we go.
    std::set<Region*> worklist((*(blocks.find(global))).second);
    for (std::set<Region*>::const_iterator b_it = worklist.begin(); b_it != worklist.end(); b_it++) {
      std::set<Region*>* df = (*b_it)->GetDominanceFrontier();
      for (std::set<Region*>::const_iterator df_it = df->begin(); df_it != df->end(); df_it++) {
        if ((*df_it)->InsertPhiFor(global)) {
          // Check that the dominance frontier element is in the worklist already
          // because we only want to break if the element is actually not there yet.
          if (worklist.find(*df_it) == worklist.end()) {
            worklist.insert(*df_it);
            b_it = worklist.begin();
            break;
          }
        }
      }
    }
  }
  // Pass: Build edges to the definition corresponding to each use.
  // (This corresponds to the renaming phase in traditional SSA transformations.
  // See: Cooper & Torczon, "Engineering a Compiler", second edition, page 505.)
  RenameAsSSA();
}

void SeaGraph::RenameAsSSA(Region* crt_region,
    utils::ScopedHashtable<int, InstructionNode*>* scoped_table) {
  scoped_table->OpenScope();
  // Rename phi nodes defined in the current region.
  std::vector<PhiInstructionNode*>* phis = crt_region->GetPhiNodes();
  for (std::vector<PhiInstructionNode*>::iterator phi_it = phis->begin();
      phi_it != phis->end(); phi_it++) {
    int reg_no = (*phi_it)->GetRegisterNumber();
    scoped_table->Add(reg_no, (*phi_it));
  }
  // Rename operands of instructions from the current region.
  std::vector<InstructionNode*>* instructions = crt_region->GetInstructions();
  for (std::vector<InstructionNode*>::const_iterator instructions_it = instructions->begin();
      instructions_it != instructions->end(); instructions_it++) {
    InstructionNode* current_instruction = (*instructions_it);
    // Rename uses.
    std::vector<int> used_regs = current_instruction->GetUses();
    for (std::vector<int>::const_iterator reg_it = used_regs.begin();
        reg_it != used_regs.end(); reg_it++) {
      int current_used_reg = (*reg_it);
      InstructionNode* definition = scoped_table->Lookup(current_used_reg);
      current_instruction->RenameToSSA(current_used_reg, definition);
    }
    // Update scope table with latest definitions.
    std::vector<int> def_regs = current_instruction->GetDefinitions();
    for (std::vector<int>::const_iterator reg_it = def_regs.begin();
            reg_it != def_regs.end(); reg_it++) {
      int current_defined_reg = (*reg_it);
      scoped_table->Add(current_defined_reg, current_instruction);
    }
  }
  // Fill in uses of phi functions in CFG successor regions.
  const std::vector<Region*>* successors = crt_region->GetSuccessors();
  for (std::vector<Region*>::const_iterator successors_it = successors->begin();
      successors_it != successors->end(); successors_it++) {
    Region* successor = (*successors_it);
    successor->SetPhiDefinitionsForUses(scoped_table, crt_region);
  }

  // Rename all successors in the dominators tree.
  const std::set<Region*>* dominated_nodes = crt_region->GetIDominatedSet();
  for (std::set<Region*>::const_iterator dominated_nodes_it = dominated_nodes->begin();
      dominated_nodes_it != dominated_nodes->end(); dominated_nodes_it++) {
    Region* dominated_node = (*dominated_nodes_it);
    RenameAsSSA(dominated_node, scoped_table);
  }
  scoped_table->CloseScope();
}

void SeaGraph::GenerateLLVM() {
  // Pass: Generate LLVM IR.
  CodeGenPrepassVisitor code_gen_prepass_visitor;
  std::cout << "Generating code..." << std::endl;
  std::cout << "=== PRE VISITING ===" << std::endl;
  Accept(&code_gen_prepass_visitor);
  CodeGenVisitor code_gen_visitor(code_gen_prepass_visitor.GetData());
  std::cout << "=== VISITING ===" << std::endl;
  Accept(&code_gen_visitor);
  std::cout << "=== POST VISITING ===" << std::endl;
  CodeGenPostpassVisitor code_gen_postpass_visitor(code_gen_visitor.GetData());
  Accept(&code_gen_postpass_visitor);
  code_gen_postpass_visitor.Write(std::string("my_file.llvm"));
}

void SeaGraph::CompileMethod(const art::DexFile::CodeItem* code_item,
  uint32_t class_def_idx, uint32_t method_idx, const art::DexFile& dex_file) {
  // Two passes: Builds the intermediate structure (non-SSA) of the sea-ir for the function.
  BuildMethodSeaGraph(code_item, dex_file, class_def_idx, method_idx);
  //Pass: Compute reverse post-order of regions.
  ComputeRPO();
  // Multiple passes: compute immediate dominators.
  ComputeIDominators();
  // Pass: compute downward-exposed definitions.
  ComputeDownExposedDefs();
  // Multiple Passes (iterative fixed-point algorithm): Compute reaching definitions
  ComputeReachingDefs();
  // Pass (O(nlogN)): Compute the dominance frontier for region nodes.
  ComputeDominanceFrontier();
  // Two Passes: Phi node insertion.
  ConvertToSSA();
  // Pass: Generate LLVM IR.
  GenerateLLVM();
}

void SeaGraph::ComputeDominanceFrontier() {
  for (std::vector<Region*>::iterator region_it = regions_.begin();
      region_it != regions_.end(); region_it++) {
    std::vector<Region*>* preds = (*region_it)->GetPredecessors();
    if (preds->size() > 1) {
      for (std::vector<Region*>::iterator pred_it = preds->begin();
          pred_it != preds->end(); pred_it++) {
        Region* runner = *pred_it;
        while (runner != (*region_it)->GetIDominator()) {
          runner->AddToDominanceFrontier(*region_it);
          runner = runner->GetIDominator();
        }
      }
    }
  }
}

Region* SeaGraph::GetNewRegion() {
  Region* new_region = new Region();
  AddRegion(new_region);
  return new_region;
}

void SeaGraph::AddRegion(Region* r) {
  DCHECK(r) << "Tried to add NULL region to SEA graph.";
  regions_.push_back(r);
}

/*
void SeaNode::AddSuccessor(Region* successor) {
  DCHECK(successor) << "Tried to add NULL successor to SEA node.";
  successors_.push_back(successor);
  return;
}

void SeaNode::AddPredecessor(Region* predecessor) {
  DCHECK(predecessor) << "Tried to add NULL predecessor to SEA node.";
  predecessors_.push_back(predecessor);
}
*/
void Region::AddChild(sea_ir::InstructionNode* instruction) {
  DCHECK(instruction) << "Tried to add NULL instruction to region node.";
  instructions_.push_back(instruction);
  instruction->SetRegion(this);
}

SeaNode* Region::GetLastChild() const {
  if (instructions_.size() > 0) {
    return instructions_.back();
  }
  return NULL;
}

void Region::ToDot(std::string& result) const {
  result += "\n// Region: \n" + StringId() + " [label=\"region " + StringId() + "(rpo=";
  std::stringstream ss;
  ss << rpo_;
  result.append(ss.str());
  if (NULL != GetIDominator()) {
    result += " dom=" + GetIDominator()->StringId();
  }
  result += ")\"];\n";

  // Save phi-nodes.
  for (std::vector<PhiInstructionNode*>::const_iterator cit = phi_instructions_.begin();
      cit != phi_instructions_.end(); cit++) {
    (*cit)->ToDot(result);
    result += StringId() + " -> " + (*cit)->StringId() + "; // phi-function \n";
  }

  // Save instruction nodes.
  for (std::vector<InstructionNode*>::const_iterator cit = instructions_.begin();
      cit != instructions_.end(); cit++) {
    (*cit)->ToDot(result);
    result += StringId() + " -> " + (*cit)->StringId() + "; // region -> instruction \n";
  }

  for (std::vector<Region*>::const_iterator cit = successors_.begin(); cit != successors_.end();
      cit++) {
    DCHECK(NULL != *cit) << "Null successor found for SeaNode" << GetLastChild()->StringId() << ".";
    result += GetLastChild()->StringId() + " -> " + (*cit)->StringId() + ";\n\n";
  }
  // Save reaching definitions.
  for (std::map<int, std::set<sea_ir::InstructionNode*>* >::const_iterator cit =
      reaching_defs_.begin();
      cit != reaching_defs_.end(); cit++) {
    for (std::set<sea_ir::InstructionNode*>::const_iterator
        reaching_set_it = (*cit).second->begin();
        reaching_set_it != (*cit).second->end();
        reaching_set_it++) {
      result += (*reaching_set_it)->StringId() +
         " -> " + StringId() +
         " [style=dotted]; // Reaching def.\n";
    }
  }
  // Save dominance frontier.
  for (std::set<Region*>::const_iterator cit = df_.begin(); cit != df_.end(); cit++) {
    result += StringId() +
        " -> " + (*cit)->StringId() +
        " [color=gray]; // Dominance frontier.\n";
  }
  result += "// End Region.\n";
}

void Region::ComputeDownExposedDefs() {
  for (std::vector<InstructionNode*>::const_iterator inst_it = instructions_.begin();
      inst_it != instructions_.end(); inst_it++) {
    int reg_no = (*inst_it)->GetResultRegister();
    std::map<int, InstructionNode*>::iterator res = de_defs_.find(reg_no);
    if ((reg_no != NO_REGISTER) && (res == de_defs_.end())) {
      de_defs_.insert(std::pair<int, InstructionNode*>(reg_no, *inst_it));
    } else {
      res->second = *inst_it;
    }
  }
  for (std::map<int, sea_ir::InstructionNode*>::const_iterator cit = de_defs_.begin();
      cit != de_defs_.end(); cit++) {
    (*cit).second->MarkAsDEDef();
  }
}

const std::map<int, sea_ir::InstructionNode*>* Region::GetDownExposedDefs() const {
  return &de_defs_;
}

std::map<int, std::set<sea_ir::InstructionNode*>* >* Region::GetReachingDefs() {
  return &reaching_defs_;
}

bool Region::UpdateReachingDefs() {
  std::map<int, std::set<sea_ir::InstructionNode*>* > new_reaching;
  for (std::vector<Region*>::const_iterator pred_it = predecessors_.begin();
      pred_it != predecessors_.end(); pred_it++) {
    // The reaching_defs variable will contain reaching defs __for current predecessor only__
    std::map<int, std::set<sea_ir::InstructionNode*>* > reaching_defs;
    std::map<int, std::set<sea_ir::InstructionNode*>* >* pred_reaching = (*pred_it)->GetReachingDefs();
    const std::map<int, InstructionNode*>* de_defs = (*pred_it)->GetDownExposedDefs();

    // The definitions from the reaching set of the predecessor
    // may be shadowed by downward exposed definitions from the predecessor,
    // otherwise the defs from the reaching set are still good.
    for (std::map<int, InstructionNode*>::const_iterator de_def = de_defs->begin();
        de_def != de_defs->end(); de_def++) {
      std::set<InstructionNode*>* solo_def;
      solo_def = new std::set<InstructionNode*>();
      solo_def->insert(de_def->second);
      reaching_defs.insert(
          std::pair<int const, std::set<InstructionNode*>*>(de_def->first, solo_def));
    }
    reaching_defs.insert(pred_reaching->begin(), pred_reaching->end());

    // Now we combine the reaching map coming from the current predecessor (reaching_defs)
    // with the accumulated set from all predecessors so far (from new_reaching).
    std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator reaching_it = reaching_defs.begin();
    for (; reaching_it != reaching_defs.end(); reaching_it++) {
      std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator crt_entry =
          new_reaching.find(reaching_it->first);
      if (new_reaching.end() != crt_entry) {
        crt_entry->second->insert(reaching_it->second->begin(), reaching_it->second->end());
      } else {
        new_reaching.insert(
            std::pair<int, std::set<sea_ir::InstructionNode*>*>(
                reaching_it->first,
                reaching_it->second) );
      }
    }
  }
  bool changed = false;
  // Because the sets are monotonically increasing,
  // we can compare sizes instead of using set comparison.
  // TODO: Find formal proof.
  int old_size = 0;
  if (-1 == reaching_defs_size_) {
    std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator reaching_it = reaching_defs_.begin();
    for (; reaching_it != reaching_defs_.end(); reaching_it++) {
      old_size += (*reaching_it).second->size();
    }
  } else {
    old_size = reaching_defs_size_;
  }
  int new_size = 0;
  std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator reaching_it = new_reaching.begin();
  for (; reaching_it != new_reaching.end(); reaching_it++) {
    new_size += (*reaching_it).second->size();
  }
  if (old_size != new_size) {
    changed = true;
  }
  if (changed) {
    reaching_defs_ = new_reaching;
    reaching_defs_size_ = new_size;
  }
  return changed;
}

bool Region::InsertPhiFor(int reg_no) {
  if (!ContainsPhiFor(reg_no)) {
    phi_set_.insert(reg_no);
    PhiInstructionNode* new_phi = new PhiInstructionNode(reg_no);
    new_phi->SetRegion(this);
    phi_instructions_.push_back(new_phi);
    return true;
  }
  return false;
}

void Region::SetPhiDefinitionsForUses(
    const utils::ScopedHashtable<int, InstructionNode*>* scoped_table, Region* predecessor) {
  int predecessor_id = -1;
  for (unsigned int crt_pred_id = 0; crt_pred_id < predecessors_.size(); crt_pred_id++) {
    if (predecessors_.at(crt_pred_id) == predecessor) {
      predecessor_id = crt_pred_id;
    }
  }
  DCHECK_NE(-1, predecessor_id);
  for (std::vector<PhiInstructionNode*>::iterator phi_it = phi_instructions_.begin();
      phi_it != phi_instructions_.end(); phi_it++) {
    PhiInstructionNode* phi = (*phi_it);
    int reg_no = phi->GetRegisterNumber();
    InstructionNode* definition = scoped_table->Lookup(reg_no);
    phi->RenameToSSA(reg_no, definition, predecessor_id);
  }
}

std::vector<InstructionNode*> InstructionNode::Create(const art::Instruction* in) {
  std::vector<InstructionNode*> sea_instructions;
  switch (in->Opcode()) {
    case art::Instruction::CONST_4:
      sea_instructions.push_back(new ConstInstructionNode(in));
      break;
    case art::Instruction::RETURN:
      sea_instructions.push_back(new ReturnInstructionNode(in));
      break;
    case art::Instruction::IF_NE:
      sea_instructions.push_back(new IfNeInstructionNode(in));
      break;
    case art::Instruction::ADD_INT_LIT8:
      sea_instructions.push_back(new UnnamedConstInstructionNode(in, in->VRegB_22b()));
      sea_instructions.push_back(new AddIntLitInstructionNode(in));
      break;
    case art::Instruction::MOVE_RESULT:
      sea_instructions.push_back(new MoveResultInstructionNode(in));
      break;
    case art::Instruction::INVOKE_STATIC:
      sea_instructions.push_back(new InvokeStaticInstructionNode(in));
      break;
    case art::Instruction::ADD_INT:
      sea_instructions.push_back(new AddIntInstructionNode(in));
      break;
    case art::Instruction::GOTO:
      sea_instructions.push_back(new GotoInstructionNode(in));
      break;
    case art::Instruction::IF_EQZ:
      sea_instructions.push_back(new IfEqzInstructionNode(in));
      break;
    default:
      // Default, generic IR instruction node; default case should never be reached
      // when support for all instructions ahs been added.
      sea_instructions.push_back(new InstructionNode(in));
  }
  return sea_instructions;
}

void InstructionNode::ToDot(std::string& result) const {
  result += "// Instruction ("+StringId()+"): \n" + StringId() +
      " [label=\"" + instruction_->DumpString(NULL) + "\"";
  if (de_def_) {
    result += "style=bold";
  }
  result += "];\n";
  // SSA definitions:
  for (std::map<int, InstructionNode* >::const_iterator def_it = definition_edges_.begin();
      def_it != definition_edges_.end(); def_it++) {
    if (NULL != def_it->second) {
      result += def_it->second->StringId() + " -> " + StringId() +"[color=red,label=\"";
      std::stringstream ss;
      ss << def_it->first;
      result.append(ss.str());
      result += "\"] ; // ssa edge\n";
    }
  }
}

void InstructionNode::MarkAsDEDef() {
  de_def_ = true;
}

int InstructionNode::GetResultRegister() const {
  if (instruction_->HasVRegA() && InstructionTools::IsDefinition(instruction_)) {
    return instruction_->VRegA();
  }
  return NO_REGISTER;
}

std::vector<int> InstructionNode::GetDefinitions() const {
  // TODO: Extend this to handle instructions defining more than one register (if any)
  // The return value should be changed to pointer to field then; for now it is an object
  // so that we avoid possible memory leaks from allocating objects dynamically.
  std::vector<int> definitions;
  int result = GetResultRegister();
  if (NO_REGISTER != result) {
    definitions.push_back(result);
  }
  return definitions;
}

std::vector<int> InstructionNode::GetUses() {
  std::vector<int> uses; // Using vector<> instead of set<> because order matters.
  if (!InstructionTools::IsDefinition(instruction_) && (instruction_->HasVRegA())) {
    int vA = instruction_->VRegA();
    uses.push_back(vA);
  }
  if (instruction_->HasVRegB()) {
    int vB = instruction_->VRegB();
    uses.push_back(vB);
  }
  if (instruction_->HasVRegC()) {
    int vC = instruction_->VRegC();
    uses.push_back(vC);
  }
  return uses;
}

void PhiInstructionNode::ToDot(std::string& result) const {
  result += "// PhiInstruction: \n" + StringId() +
      " [label=\"" + "PHI(";
  std::stringstream phi_reg_stream;
  phi_reg_stream << register_no_;
  result.append(phi_reg_stream.str());
  result += ")\"";
  result += "];\n";

  for (std::vector<std::vector<InstructionNode*>*>::const_iterator pred_it = definition_edges_.begin();
      pred_it != definition_edges_.end(); pred_it++) {
    std::vector<InstructionNode*>* defs_from_pred = *pred_it;
    for (std::vector<InstructionNode* >::const_iterator def_it = defs_from_pred->begin();
        def_it != defs_from_pred->end(); def_it++) {
        result += (*def_it)->StringId() + " -> " + StringId() +"[color=red,label=\"vR = ";
        std::stringstream ss;
        ss << GetRegisterNumber();
        result.append(ss.str());
        result += "\"] ; // phi-ssa edge\n";
    }
  }
}
} // end namespace sea_ir
