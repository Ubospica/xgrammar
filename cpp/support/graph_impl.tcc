/*!
 * Copyright (c) 2024 by Contributors
 * \file xgrammar/support/graph_impl.tcc
 * \brief The implementation of graph data structure. This file contains template implementations
 * and is included in graph.h.
 */
#ifndef XGRAMMAR_SUPPORT_GRAPH_IMPL_H_
#define XGRAMMAR_SUPPORT_GRAPH_IMPL_H_

#include <picojson.h>

#include <cstdint>
#include <queue>
#include <vector>

// "graph.h" will not actually be included; it’s just for the IDE’s static checking.
#include "graph.h"
#include "logging.h"

namespace xgrammar {

template <typename LabelType>
int32_t Graph<LabelType>::GetNextEdgeFromTo(int32_t src, int32_t dst, int32_t last_edge_id) const {
  int32_t eid = last_edge_id == INVALID_EDGE_ID ? FirstOutEdge(src) : NextOutEdge(last_edge_id);
  for (; eid != INVALID_EDGE_ID; eid = NextOutEdge(eid)) {
    if (edges_[eid].dst == dst) {
      return eid;
    }
  }
  return INVALID_EDGE_ID;
}

template <typename LabelType>
void Graph<LabelType>::RemoveEdge(int32_t edge_id) {
  RemoveOutEdge(edges_[edge_id].src, edge_id);
  RemoveInEdge(edges_[edge_id].dst, edge_id);
}

template <typename LabelType>
void Graph<LabelType>::RemoveOutEdge(int32_t src, int32_t edge_id) {
  int32_t prev_out_edge_id = INVALID_EDGE_ID;
  for (int32_t eid = FirstOutEdge(src); eid != INVALID_EDGE_ID; eid = NextOutEdge(eid)) {
    if (eid == edge_id) {
      if (prev_out_edge_id == INVALID_EDGE_ID) {
        adj_heads_[src].first = NextOutEdge(eid);
      } else {
        edges_[prev_out_edge_id].next_out_edge_id = NextOutEdge(eid);
      }
      break;
    }
    prev_out_edge_id = eid;
  }
  --out_in_degrees_[src].first;
}

template <typename LabelType>
void Graph<LabelType>::RemoveInEdge(int32_t dst, int32_t edge_id) {
  int32_t prev_in_edge_id = INVALID_EDGE_ID;
  for (int32_t eid = FirstInEdge(dst); eid != INVALID_EDGE_ID; eid = NextInEdge(eid)) {
    if (eid == edge_id) {
      if (prev_in_edge_id == INVALID_EDGE_ID) {
        adj_heads_[dst].second = NextInEdge(eid);
      } else {
        edges_[prev_in_edge_id].next_in_edge_id = NextInEdge(eid);
      }
      break;
    }
    prev_in_edge_id = eid;
  }
  --out_in_degrees_[dst].second;
}

template <typename LabelType>
void Graph<LabelType>::Coalesce(int32_t lhs, int32_t rhs) {
  XGRAMMAR_DCHECK(lhs != rhs) << "Cannot coalesce an edge to itself";

  for (int32_t eid = FirstInEdge(rhs); eid != INVALID_EDGE_ID; eid = NextInEdge(eid)) {
    const auto& edge = edges_[eid];
    RemoveOutEdge(edge.src, eid);
    XGRAMMAR_DCHECK(edge.src != rhs) << "Self-loop detected on dst node";
    if (edge.src != lhs) {
      AddEdge(edge.src, lhs, edge.label);
    }
  }

  for (int32_t eid = FirstOutEdge(rhs); eid != INVALID_EDGE_ID; eid = NextOutEdge(eid)) {
    const auto& edge = edges_[eid];
    RemoveInEdge(edge.dst, eid);
    XGRAMMAR_DCHECK(edge.dst != rhs) << "Self-loop detected on dst node";
    if (edge.dst != lhs) {
      AddEdge(lhs, edge.dst, edge.label);
    }
  }

  adj_heads_[rhs] = std::make_pair(INVALID_EDGE_ID, INVALID_EDGE_ID);
  out_in_degrees_[rhs] = std::make_pair(0, 0);

  XGRAMMAR_DCHECK(WellFormed()) << "Graph is not well-formed after coalescing";
}

template <typename LabelType>
std::vector<int32_t> Graph<LabelType>::Simplify(const std::vector<int32_t>& start_nodes) {
  XGRAMMAR_DCHECK(WellFormed()) << "Graph is not well-formed before simplifying";
  std::vector<int32_t> node_mapping(NumNodes(), -1);
  Graph<LabelType> new_graph;

  std::queue<int32_t> queue;
  for (int32_t start : start_nodes) {
    if (node_mapping[start] == -1) {
      queue.push(start);
      node_mapping[start] = new_graph.AddNode();

      while (!queue.empty()) {
        int32_t current = queue.front();
        queue.pop();

        for (int32_t eid = FirstOutEdge(current); eid != INVALID_EDGE_ID; eid = NextOutEdge(eid)) {
          const auto& edge = GetEdgeFromId(eid);
          int32_t neighbor = edge.dst;
          if (node_mapping[neighbor] == -1) {
            node_mapping[neighbor] = new_graph.AddNode();
            queue.push(neighbor);
          }

          new_graph.AddEdge(node_mapping[current], node_mapping[neighbor], edge.label);
        }
      }
    }
  }

  *this = std::move(new_graph);

  XGRAMMAR_DCHECK(WellFormed()) << "Graph is not well-formed after simplifying";

  // Update start_nodes to reflect new node indices
  std::vector<int32_t> new_start_nodes;
  new_start_nodes.reserve(start_nodes.size());
  for (auto node : start_nodes) {
    if (node_mapping[node] != -1) {
      new_start_nodes.push_back(node_mapping[node]);
    }
  }
  return new_start_nodes;
}

template <typename LabelType>
std::ostream& operator<<(std::ostream& os, const Graph<LabelType>& graph) {
  os << "Graph(num_nodes=" << graph.NumNodes() << ", edges={";
  for (int32_t i = 0; i < graph.NumNodes(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    os << i << ": [";
    int cnt = 0;
    for (int32_t eid = graph.FirstOutEdge(i); eid != Graph<LabelType>::INVALID_EDGE_ID;
         eid = graph.NextOutEdge(eid)) {
      const auto& edge = graph.GetEdgeFromId(eid);
      if (cnt != 0) {
        os << ", ";
      }
      os << "(" << edge.dst << ", " << edge.label << ")";
      ++cnt;
    }
    os << "]";
  }
  os << "})";
  return os;
}

template <typename LabelType>
bool Graph<LabelType>::WellFormed() const {
  for (int32_t i = 0; i < NumNodes(); ++i) {
    int out_degree = 0;
    for (int32_t eid = FirstOutEdge(i); eid != INVALID_EDGE_ID; eid = NextOutEdge(eid)) {
      // The src of an edge should be the current node
      if (edges_[eid].src != i) {
        XGRAMMAR_LOG(WARNING) << "The src of an edge should be the current node. Node: " << i
                              << ", Edge: " << eid;
        return false;
      }

      ++out_degree;

      // The edge should be referenced by the dst node as an in-edge
      bool found = false;
      for (int32_t eid2 = FirstInEdge(edges_[eid].dst); eid2 != INVALID_EDGE_ID;
           eid2 = NextInEdge(eid2)) {
        if (eid2 == eid) {
          found = true;
          break;
        }
      }
      if (!found) {
        XGRAMMAR_LOG(WARNING
        ) << "The edge should be referenced by the dst node as an in-edge. Node: "
          << i << ", Edge: " << eid;
        return false;
      }
    }
    // Check out-degree
    if (OutDegree(i) != out_degree) {
      XGRAMMAR_LOG(WARNING) << "Out-degree mismatch. Node: " << i << ", Expected: " << OutDegree(i)
                            << ", Actual: " << out_degree;
      return false;
    }
  }

  for (int32_t i = 0; i < NumNodes(); ++i) {
    int in_degree = 0;
    for (int32_t eid = FirstInEdge(i); eid != INVALID_EDGE_ID; eid = NextInEdge(eid)) {
      if (edges_[eid].dst != i) {
        XGRAMMAR_LOG(WARNING) << "The dst of an edge should be the current node. Node: " << i
                              << ", Edge: " << eid;
        return false;
      }
      ++in_degree;

      // The edge should be referenced by the src node as an out-edge
      bool found = false;
      for (int32_t eid2 = FirstOutEdge(edges_[eid].src); eid2 != INVALID_EDGE_ID;
           eid2 = NextOutEdge(eid2)) {
        if (eid2 == eid) {
          found = true;
          break;
        }
      }
      if (!found) {
        XGRAMMAR_LOG(WARNING
        ) << "The edge should be referenced by the src node as an out-edge. Node: "
          << i << ", Edge: " << eid;
        return false;
      }
    }

    // Check in-degree
    if (InDegree(i) != in_degree) {
      XGRAMMAR_LOG(WARNING) << "In-degree mismatch. Node: " << i << ", Expected: " << InDegree(i)
                            << ", Actual: " << in_degree;
      return false;
    }
  }
  return true;
}

template <typename LabelType>
picojson::value Graph<LabelType>::Serialize() const {
  picojson::object obj;
  // Serialize edges
  picojson::array edges_array;
  for (const auto& edge : edges_) {
    picojson::array edge_array;
    edge_array.push_back(picojson::value(static_cast<int64_t>(edge.label)));
    edge_array.push_back(picojson::value(static_cast<int64_t>(edge.src)));
    edge_array.push_back(picojson::value(static_cast<int64_t>(edge.dst)));
    edge_array.push_back(picojson::value(static_cast<int64_t>(edge.next_out_edge_id)));
    edge_array.push_back(picojson::value(static_cast<int64_t>(edge.next_in_edge_id)));
    edges_array.push_back(picojson::value(edge_array));
  }
  obj["edges"] = picojson::value(edges_array);

  // Serialize adj_heads
  picojson::array adj_heads_array;
  for (const auto& pair : adj_heads_) {
    picojson::array pair_array;
    pair_array.push_back(picojson::value(static_cast<int64_t>(pair.first)));
    pair_array.push_back(picojson::value(static_cast<int64_t>(pair.second)));
    adj_heads_array.push_back(picojson::value(pair_array));
  }
  obj["adj_heads"] = picojson::value(adj_heads_array);

  // Serialize out_in_degrees
  picojson::array out_in_degrees_array;
  for (const auto& pair : out_in_degrees_) {
    picojson::array pair_array;
    pair_array.push_back(picojson::value(static_cast<int64_t>(pair.first)));
    pair_array.push_back(picojson::value(static_cast<int64_t>(pair.second)));
    out_in_degrees_array.push_back(picojson::value(pair_array));
  }
  obj["out_in_degrees"] = picojson::value(out_in_degrees_array);
  return picojson::value(obj);
}

template <typename LabelType>
FSM FSM::CreateWithLabel(Graph<LabelType>* graph, LabelType label) {
  int32_t start_node = graph->AddNode();
  int32_t end_node = graph->AddNode();
  graph->AddEdge(start_node, end_node, label);
  return FSM{start_node, end_node};
}

template <typename LabelType>
FSM FSM::Concat(Graph<LabelType>* graph, FSM lhs, FSM rhs, LabelType epsilon_label) {
  graph->AddEdge(lhs.end_node, rhs.start_node, epsilon_label);
  return FSM{lhs.start_node, rhs.end_node};
}

template <typename LabelType>
FSM FSM::Alternative(
    Graph<LabelType>* graph, const std::vector<FSM>& fsms, LabelType epsilon_label
) {
  int32_t start_node = graph->AddNode();
  int32_t end_node = graph->AddNode();
  for (const auto& fsm : fsms) {
    graph->AddEdge(start_node, fsm.start_node, epsilon_label);
    graph->AddEdge(fsm.end_node, end_node, epsilon_label);
  }
  return FSM{start_node, end_node};
}

template <typename LabelType>
FSM FSM::StarQuantifier(Graph<LabelType>* graph, FSM fsm, LabelType epsilon_label) {
  graph->AddEdge(fsm.end_node, fsm.start_node, epsilon_label);
  return FSM{fsm.start_node, fsm.start_node};
}

template <typename LabelType>
FSM FSM::PlusQuantifier(Graph<LabelType>* graph, FSM fsm, LabelType epsilon_label) {
  graph->AddEdge(fsm.end_node, fsm.start_node, epsilon_label);
  return FSM{fsm.start_node, fsm.end_node};
}

template <typename LabelType>
FSM FSM::QuestionQuantifier(Graph<LabelType>* graph, FSM fsm, LabelType epsilon_label) {
  graph->AddEdge(fsm.start_node, fsm.end_node, epsilon_label);
  return FSM{fsm.start_node, fsm.end_node};
}

}  // namespace xgrammar

#endif  // XGRAMMAR_SUPPORT_GRAPH_IMPL_H_
