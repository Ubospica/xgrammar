#include <gtest/gtest.h>

#include <sstream>

#include "support/graph.h"

using namespace xgrammar;

TEST(XGrammarGraphTest, CreateGraph) {
  // Create a graph with integer labels
  Graph<int> graph;

  // Add nodes
  int32_t node0 = graph.AddNode();
  int32_t node1 = graph.AddNode();
  int32_t node2 = graph.AddNode();

  // Add edges
  graph.AddEdge(node0, node1, 10);
  graph.AddEdge(node1, node2, 20);
  graph.AddEdge(node2, node0, 30);
  graph.AddEdge(node1, node2, 40);

  EXPECT_TRUE(graph.WellFormed());
  std::stringstream ss;
  ss << graph;
  EXPECT_EQ(
      ss.str(), "Graph(num_nodes=3, edges={0: [(1, 10)], 1: [(2, 40), (2, 20)], 2: [(0, 30)]})"
  );
}

TEST(XGrammarGraphTest, RemoveEdge) {
  Graph<int> graph;

  // Add nodes
  int32_t node0 = graph.AddNode();
  int32_t node1 = graph.AddNode();
  int32_t node2 = graph.AddNode();

  int32_t edge0 = graph.AddEdge(node0, node1, 10);
  int32_t edge1 = graph.AddEdge(node1, node2, 20);
  graph.AddEdge(node2, node0, 30);
  int32_t edge3 = graph.AddEdge(node1, node2, 40);

  graph.RemoveEdge(edge3);
  EXPECT_TRUE(graph.WellFormed());
  std::stringstream ss;
  ss << graph;
  EXPECT_EQ(ss.str(), "Graph(num_nodes=3, edges={0: [(1, 10)], 1: [(2, 20)], 2: [(0, 30)]})");

  graph.RemoveEdge(edge0);
  EXPECT_TRUE(graph.WellFormed());
  ss.str("");
  ss << graph;
  EXPECT_EQ(ss.str(), "Graph(num_nodes=3, edges={0: [], 1: [(2, 20)], 2: [(0, 30)]})");

  graph.RemoveEdge(edge1);
  EXPECT_TRUE(graph.WellFormed());
  ss.str("");
  ss << graph;
  EXPECT_EQ(ss.str(), "Graph(num_nodes=3, edges={0: [], 1: [], 2: [(0, 30)]})");
}

TEST(XGrammarGraphTest, Coalesce) {
  Graph<int> graph;

  // Add nodes
  int32_t node0 = graph.AddNode();
  int32_t node1 = graph.AddNode();
  int32_t node2 = graph.AddNode();
  int32_t node3 = graph.AddNode();

  graph.AddEdge(node0, node1, 10);
  graph.AddEdge(node1, node2, 20);
  graph.AddEdge(node2, node0, 30);
  graph.AddEdge(node1, node3, 40);

  graph.Coalesce(node0, node1);
  EXPECT_TRUE(graph.WellFormed());
  std::stringstream ss;
  ss << graph;
  EXPECT_EQ(
      ss.str(), "Graph(num_nodes=4, edges={0: [(2, 20), (3, 40)], 1: [], 2: [(0, 30)], 3: []})"
  );

  graph.Coalesce(node0, node3);
  EXPECT_TRUE(graph.WellFormed());
  ss.str("");
  ss << graph;
  EXPECT_EQ(ss.str(), "Graph(num_nodes=4, edges={0: [(2, 20)], 1: [], 2: [(0, 30)], 3: []})");
}

TEST(XGrammarGraphTest, Simplify) {
  Graph<int> graph;

  // Create a graph with multiple partitions
  int32_t node0 = graph.AddNode();
  int32_t node1 = graph.AddNode();
  int32_t node2 = graph.AddNode();
  int32_t node3 = graph.AddNode();
  int32_t node4 = graph.AddNode();
  int32_t node5 = graph.AddNode();

  // Partition 1
  auto edge0 = graph.AddEdge(node0, node1, 10);
  graph.AddEdge(node1, node2, 20);
  graph.AddEdge(node2, node0, 30);

  // Partition 2
  graph.AddEdge(node3, node4, 40);
  graph.AddEdge(node4, node5, 50);
  graph.AddEdge(node5, node3, 60);

  // Remove an edge from partition 1
  graph.RemoveEdge(edge0);

  // Simplify the graph
  std::vector<int32_t> new_start_nodes = graph.Simplify({node2, node1});

  // Check if the graph is well-formed after simplification
  EXPECT_TRUE(graph.WellFormed());

  // Check if the number of nodes has been reduced (partition 2 should be removed)
  EXPECT_EQ(graph.NumNodes(), 3);
  EXPECT_EQ(graph.NumEdges(), 2);

  // Check if the start node is preserved and updated
  EXPECT_EQ(new_start_nodes.size(), 2);
  EXPECT_EQ(new_start_nodes[0], 0);  // The new index of node2 should be 0
  EXPECT_EQ(new_start_nodes[1], 2);  // The new index of node1 should be 2
  // Check the structure of the simplified graph
  std::stringstream ss;
  ss << graph;
  EXPECT_EQ(ss.str(), "Graph(num_nodes=3, edges={0: [(1, 30)], 1: [], 2: [(0, 20)]})");
}
