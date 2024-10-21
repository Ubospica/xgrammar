/*!
 * Copyright (c) 2024 by Contributors
 * \file xgrammar/support/graph.h
 * \brief The header for graph data structure.
 */
#ifndef XGRAMMAR_SUPPORT_GRAPH_H_
#define XGRAMMAR_SUPPORT_GRAPH_H_

#include <picojson.h>

#include <cstdint>
#include <queue>
#include <vector>

#include "logging.h"

namespace xgrammar {

template <typename LabelType>
class Graph {
 public:
  Graph(int32_t num_nodes = 0) : adj_heads_(num_nodes), out_in_degrees_(num_nodes) {}
  Graph(const Graph&) = default;
  Graph(Graph&&) = default;
  Graph& operator=(const Graph&) = default;
  Graph& operator=(Graph&&) = default;

  static constexpr int32_t INVALID_EDGE_ID = -1;

  struct Edge {
    LabelType label;
    int32_t src;
    int32_t dst;
    int32_t next_out_edge_id;
    int32_t next_in_edge_id;
    friend std::ostream& operator<<(std::ostream& os, const Edge& edge) {
      os << "Edge(label=" << edge.label << ", src=" << edge.src << ", dst=" << edge.dst
         << ", next_out_edge_id=" << edge.next_out_edge_id
         << ", next_in_edge_id=" << edge.next_in_edge_id << ")";
      return os;
    }
  };

  int32_t FirstOutEdge(int32_t node_id) const { return adj_heads_[node_id].first; }
  int32_t NextOutEdge(int32_t edge_id) const { return edges_[edge_id].next_out_edge_id; }

  int32_t FirstInEdge(int32_t node_id) const { return adj_heads_[node_id].second; }
  int32_t NextInEdge(int32_t edge_id) const { return edges_[edge_id].next_in_edge_id; }

  int32_t OutDegree(int32_t node_id) const { return out_in_degrees_[node_id].first; }
  int32_t InDegree(int32_t node_id) const { return out_in_degrees_[node_id].second; }

  int32_t NumNodes() const { return adj_heads_.size(); }
  int32_t NumEdges() const { return edges_.size(); }

  int32_t GetNextEdgeFromTo(int32_t src, int32_t dst, int32_t last_edge_id) const;

  Edge& GetEdgeFromId(int32_t edge_id) { return edges_[edge_id]; }
  const Edge& GetEdgeFromId(int32_t edge_id) const { return edges_[edge_id]; }

  int32_t AddNode() {
    adj_heads_.emplace_back(std::make_pair(INVALID_EDGE_ID, INVALID_EDGE_ID));
    out_in_degrees_.emplace_back(std::make_pair(0, 0));
    return adj_heads_.size() - 1;
  }

  int32_t AddEdge(int32_t src, int32_t dst, LabelType label) {
    edges_.emplace_back(Edge{label, src, dst, adj_heads_[src].first, adj_heads_[dst].second});
    ++out_in_degrees_[src].first;
    ++out_in_degrees_[dst].second;
    adj_heads_[src].first = edges_.size() - 1;
    adj_heads_[dst].second = edges_.size() - 1;
    return edges_.size() - 1;
  }

  void RemoveEdge(int32_t edge_id);
  void Coalesce(int32_t lhs, int32_t rhs);
  std::vector<int32_t> Simplify(const std::vector<int32_t>& start_nodes);

  picojson::value Serialize() const;
  // static Graph<LabelType> Deserialize(const picojson::value& value);

  bool WellFormed() const;

  template <typename LabelType>
  friend std::ostream& operator<<(std::ostream& os, const Graph<LabelType>& graph);

 private:
  void RemoveOutEdge(int32_t src, int32_t edge_id);
  void RemoveInEdge(int32_t dst, int32_t edge_id);

  std::vector<Edge> edges_;
  std::vector<std::pair<int32_t, int32_t>> adj_heads_;
  std::vector<std::pair<int32_t, int32_t>> out_in_degrees_;
};

struct FSM {
  int32_t start_node;
  int32_t end_node;

  template <typename LabelType>
  static FSM CreateWithLabel(Graph<LabelType>* graph, LabelType label);
  template <typename LabelType>
  static FSM Concat(Graph<LabelType>* graph, FSM lhs, FSM rhs, LabelType epsilon_label);
  template <typename LabelType>
  static FSM Alternative(
      Graph<LabelType>* graph, const std::vector<FSM>& fsms, LabelType epsilon_label
  );
  template <typename LabelType>
  static FSM StarQuantifier(Graph<LabelType>* graph, FSM fsm, LabelType epsilon_label);
  template <typename LabelType>
  static FSM PlusQuantifier(Graph<LabelType>* graph, FSM fsm, LabelType epsilon_label);
  template <typename LabelType>
  static FSM QuestionQuantifier(Graph<LabelType>* graph, FSM fsm, LabelType epsilon_label);
};

}  // namespace xgrammar

/****************** Template Implementations ******************/
#include "graph_impl.tcc"

#endif  // XGRAMMAR_SUPPORT_GRAPH_H_
