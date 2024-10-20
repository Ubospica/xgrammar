/*!
 * Copyright (c) 2024 by Contributors
 * \file xgrammar/support/dynamic_bitset.h
 * \brief The header for utilities used in grammar-guided generation.
 */
#ifndef XGRAMMAR_SUPPORT_CSR_ARRAY_H_
#define XGRAMMAR_SUPPORT_CSR_ARRAY_H_

#include <picojson.h>

#include <cstdint>
#include <vector>

#include "logging.h"

namespace xgrammar {

constexpr int32_t INVALID_EDGE_ID = -1;

template <typename LabelType>
class Graph {
 public:
  Graph() = default;
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

  bool WellFormed() const;

  picojson::value Serialize() const;
  // static Graph<LabelType> Deserialize(const picojson::value& value);

  template <typename F>
  friend std::ostream& operator<<(std::ostream& os, const Graph<F>& graph);

 private:
  void RemoveOutEdge(int32_t src, int32_t edge_id);
  void RemoveInEdge(int32_t dst, int32_t edge_id);

  std::vector<Edge> edges_;
  std::vector<std::pair<int32_t, int32_t>> adj_heads_;
  std::vector<std::pair<int32_t, int32_t>> out_in_degrees_;
};

template <typename T>
int32_t Graph<T>::GetNextEdgeFromTo(int32_t src, int32_t dst, int32_t last_edge_id) const {
  int32_t eid = last_edge_id == INVALID_EDGE_ID ? FirstOutEdge(src) : NextOutEdge(last_edge_id);
  for (; eid != INVALID_EDGE_ID; eid = NextOutEdge(eid)) {
    if (edges_[eid].dst == dst) {
      return eid;
    }
  }
  return INVALID_EDGE_ID;
}

template <typename T>
void Graph<T>::RemoveEdge(int32_t edge_id) {
  RemoveOutEdge(edges_[edge_id].src, edge_id);
  RemoveInEdge(edges_[edge_id].dst, edge_id);
}

template <typename T>
void Graph<T>::RemoveOutEdge(int32_t src, int32_t edge_id) {
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

template <typename T>
void Graph<T>::RemoveInEdge(int32_t dst, int32_t edge_id) {
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

template <typename T>
void Graph<T>::Coalesce(int32_t lhs, int32_t rhs) {
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

template <typename T>
std::ostream& operator<<(std::ostream& os, const Graph<T>& graph) {
  os << "Graph(num_nodes=" << graph.NumNodes() << ", edges={";
  for (int32_t i = 0; i < graph.NumNodes(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    os << i << ": [";
    int cnt = 0;
    for (int32_t eid = graph.FirstOutEdge(i); eid != Graph<T>::INVALID_EDGE_ID;
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

template <typename T>
bool Graph<T>::WellFormed() const {
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

template <typename T>
picojson::value Graph<T>::Serialize() const {
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

}  // namespace xgrammar

#endif  // XGRAMMAR_SUPPORT_DYNAMIC_BITSET_H_
