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

#ifndef ART_COMPILER_SEA_IR_SEA_NODE_H_
#define ART_COMPILER_SEA_IR_SEA_NODE_H_

namespace sea_ir {
class Region;
class IRVisitor;

class IVisitable {
 public:
  virtual void Accept(IRVisitor* visitor) = 0;
  virtual ~IVisitable() {}
};

// This abstract class provides the essential services that
// we want each SEA IR element should have.
// At the moment, these are:
// - an id and corresponding string representation.
// - a .dot graph language representation for .dot output.
//
// Note that SEA IR nodes could also be Regions, Projects
// which are not instructions.
class SeaNode: public IVisitable {
 public:
  explicit SeaNode():id_(GetNewId()), string_id_() {
    std::stringstream ss;
    ss << id_;
    string_id_.append(ss.str());
  }
  // Adds CFG predecessors and successors to each block.
  void AddSuccessor(Region* successor);
  void AddPredecessor(Region* predecesor);

  // Returns the id of the current block as string
  const std::string& StringId() const {
    return string_id_;
  }
  // Returns the id of this node as int. The id is supposed to be unique among
  // all instances of all subclasses of this class.
  int Id() const {
    return id_;
  }
  // Appends to @result a dot language formatted string representing the node and
  //    (by convention) outgoing edges, so that the composition of theToDot() of all nodes
  //    builds a complete dot graph, but without prolog ("digraph {") and epilog ("}").
  virtual void ToDot(std::string& result) const = 0;

  virtual ~SeaNode() {}

 protected:
  static int GetNewId() {
    return current_max_node_id_++;
  }

  const int id_;
  std::string string_id_;

 private:
  static int current_max_node_id_;
};
} // end namespace sea_ir
#endif  // ART_COMPILER_SEA_IR_SEA_NODE_H_
