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
