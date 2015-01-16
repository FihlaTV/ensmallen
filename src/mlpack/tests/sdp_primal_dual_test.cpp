/**
 * @file sdp_primal_dual_test.cpp
 * @author Stephen Tu
 *
 */
#include <mlpack/core.hpp>
#include <mlpack/core/optimizers/sdp/sdp.hpp>
#include <mlpack/core/optimizers/sdp/primal_dual.hpp>

#include <boost/test/unit_test.hpp>
#include "old_boost_test_definitions.hpp"

using namespace mlpack;
using namespace mlpack::optimization;

class UndirectedGraph
{
 public:

  UndirectedGraph() {}

  size_t NumVertices() const { return numVertices; }
  size_t NumEdges() const { return edges.n_cols; }

  const arma::umat& Edges() const { return edges; }
  const arma::vec& Weights() const { return weights; }

  void Laplacian(arma::sp_mat& laplacian) const
  {
    laplacian.zeros(numVertices, numVertices);

    for (size_t i = 0; i < edges.n_cols; ++i)
    {
      laplacian(edges(0, i), edges(1, i)) = -weights(i);
      laplacian(edges(1, i), edges(0, i)) = -weights(i);
    }

    for (size_t i = 0; i < numVertices; ++i)
    {
      laplacian(i, i) = -arma::accu(laplacian.row(i));
    }
  }

  static void LoadFromEdges(UndirectedGraph& g,
                            const std::string& edgesFilename,
                            bool transposeEdges)
  {
    data::Load(edgesFilename, g.edges, true, transposeEdges);
    if (g.edges.n_rows != 2)
      Log::Fatal << "Invalid datafile" << std::endl;
    g.weights.ones(g.edges.n_cols);
    g.ComputeVertices();
  }

  static void LoadFromEdgesAndWeights(UndirectedGraph& g,
                                      const std::string& edgesFilename,
                                      bool transposeEdges,
                                      const std::string& weightsFilename,
                                      bool transposeWeights)
  {
    data::Load(edgesFilename, g.edges, true, transposeEdges);
    if (g.edges.n_rows != 2)
      Log::Fatal << "Invalid datafile" << std::endl;
    data::Load(weightsFilename, g.weights, true, transposeWeights);
    if (g.weights.n_elem != g.edges.n_cols)
      Log::Fatal << "Size mismatch" << std::endl;
    g.ComputeVertices();
  }

  static void ErdosRenyiRandomGraph(UndirectedGraph& g,
                                    size_t numVertices,
                                    double edgeProbability,
                                    bool weighted,
                                    bool selfLoops = false)
  {
    if (edgeProbability < 0. || edgeProbability > 1.)
      Log::Fatal << "edgeProbability not in [0, 1]" << std::endl;

    std::vector<std::pair<size_t, size_t>> edges;
    std::vector<double> weights;

    for (size_t i = 0; i < numVertices; i ++)
    {
      for (size_t j = (selfLoops ? i : i + 1); j < numVertices; j++)
      {
        if (math::Random() > edgeProbability)
          continue;
        edges.emplace_back(i, j);
        weights.push_back(weighted ? math::Random() : 1.);
      }
    }

    g.edges.set_size(2, edges.size());
    for (size_t i = 0; i < edges.size(); i++)
    {
      g.edges(0, i) = edges[i].first;
      g.edges(1, i) = edges[i].second;
    }
    g.weights = arma::vec(weights);

    g.numVertices = numVertices;
  }

 private:

  void ComputeVertices()
  {
    numVertices = max(max(edges)) + 1;
  }

  arma::umat edges;
  arma::vec weights;
  size_t numVertices;
};

static inline SDP
ConstructMaxCutSDPFromGraph(const UndirectedGraph& g)
{
  SDP sdp(g.NumVertices(), g.NumVertices(), 0);
  g.Laplacian(sdp.SparseC());
  sdp.SparseC() *= -1;
  for (size_t i = 0; i < g.NumVertices(); i++)
  {
    sdp.SparseA()[i].zeros(g.NumVertices(), g.NumVertices());
    sdp.SparseA()[i](i, i) = 1.;
  }
  sdp.SparseB().ones();
  return sdp;
}

static inline SDP
ConstructLovaszThetaSDPFromGraph(const UndirectedGraph& g)
{
  SDP sdp(g.NumVertices(), g.NumEdges() + 1, 0);
  sdp.DenseC().ones();
  sdp.DenseC() *= -1.;
  sdp.SparseA()[0].eye(g.NumVertices(), g.NumVertices());
  for (size_t i = 0; i < g.NumEdges(); i++)
  {
    sdp.SparseA()[i + 1].zeros(g.NumVertices(), g.NumVertices());
    sdp.SparseA()[i + 1](g.Edges()(0, i), g.Edges()(1, i)) = 1.;
    sdp.SparseA()[i + 1](g.Edges()(1, i), g.Edges()(0, i)) = 1.;
  }
  sdp.SparseB().zeros();
  sdp.SparseB()[0] = 1.;
  return sdp;
}

// TODO: does arma have a builtin way to do this?
static inline arma::mat
Diag(const arma::vec& diag)
{
  arma::mat ret;
  ret.zeros(diag.n_elem, diag.n_elem);
  for (size_t i = 0; i < diag.n_elem; i++)
  {
    ret(i, i) = diag(i);
  }
  return ret;
}

static inline SDP
ConstructMaxCutSDPFromLaplacian(const std::string& laplacianFilename)
{
  arma::mat laplacian;
  data::Load(laplacianFilename, laplacian, true, false);
  if (laplacian.n_rows != laplacian.n_cols)
    Log::Fatal << "laplacian not square" << std::endl;
  SDP sdp(laplacian.n_rows, laplacian.n_rows, 0);
  sdp.SparseC() = -arma::sp_mat(laplacian);
  for (size_t i = 0; i < laplacian.n_rows; i++)
  {
    sdp.SparseA()[i].zeros(laplacian.n_rows, laplacian.n_rows);
    sdp.SparseA()[i](i, i) = 1.;
  }
  sdp.SparseB().ones();
  return sdp;
}


BOOST_AUTO_TEST_SUITE(SdpPrimalDualTest);

static void SolveMaxCutFeasibleSDP(const SDP& sdp)
{
  arma::mat X0, Z0;
  arma::vec ysparse0, ydense0;
  ydense0.set_size(0);

  // strictly feasible starting point
  X0.eye(sdp.N(), sdp.N());
  ysparse0 = -1.1 * arma::vec(arma::sum(arma::abs(sdp.SparseC()), 0).t());
  Z0 = -Diag(ysparse0) + sdp.SparseC();

  PrimalDualSolver solver(sdp, X0, ysparse0, ydense0, Z0);

  arma::mat X, Z;
  arma::vec ysparse, ydense;
  const auto p = solver.Optimize(X, ysparse, ydense, Z);
  BOOST_REQUIRE(p.first);
}

static void SolveMaxCutPositiveSDP(const SDP& sdp)
{
  arma::mat X0, Z0;
  arma::vec ysparse0, ydense0;
  ydense0.set_size(0);

  // infeasible, but positive starting point
  X0.randu(sdp.N(), sdp.N());
  X0 *= X0.t();
  X0 += 0.01 * arma::eye<arma::mat>(sdp.N(), sdp.N());

  ysparse0 = arma::randu<arma::vec>(sdp.NumSparseConstraints());
  Z0.eye(sdp.N(), sdp.N());

  PrimalDualSolver solver(sdp, X0, ysparse0, ydense0, Z0);

  arma::mat X, Z;
  arma::vec ysparse, ydense;
  const auto p = solver.Optimize(X, ysparse, ydense, Z);
  BOOST_REQUIRE(p.first);
}

BOOST_AUTO_TEST_CASE(SmallMaxCutSdp)
{
  SDP sdp = ConstructMaxCutSDPFromLaplacian("r10.txt");
  SolveMaxCutFeasibleSDP(sdp);
  SolveMaxCutPositiveSDP(sdp);

  UndirectedGraph g;
  UndirectedGraph::ErdosRenyiRandomGraph(g, 10, 0.3, true);
  sdp = ConstructMaxCutSDPFromGraph(g);
  SolveMaxCutFeasibleSDP(sdp);
  SolveMaxCutPositiveSDP(sdp);
}

BOOST_AUTO_TEST_CASE(SmallLovaszThetaSdp)
{
  UndirectedGraph g;
  UndirectedGraph::LoadFromEdges(g, "johnson8-4-4.csv", true);
  SDP sdp = ConstructLovaszThetaSDPFromGraph(g);

  PrimalDualSolver solver(sdp);

  arma::mat X, Z;
  arma::vec ysparse, ydense;
  const auto p = solver.Optimize(X, ysparse, ydense, Z);
  BOOST_REQUIRE(p.first);
}

static inline arma::sp_mat
RepeatBlockDiag(const arma::sp_mat& block, size_t repeat)
{
  assert(block.n_rows == block.n_cols);
  arma::sp_mat ret(block.n_rows * repeat, block.n_rows * repeat);
  ret.zeros();
  for (size_t i = 0; i < repeat; i++)
    ret(arma::span(i * block.n_rows, (i + 1) * block.n_rows - 1),
        arma::span(i * block.n_rows, (i + 1) * block.n_rows - 1)) = block;
  return ret;
}

static inline arma::sp_mat
BlockDiag(const std::vector<arma::sp_mat>& blocks)
{
  // assumes all blocks are the same size
  const size_t n = blocks.front().n_rows;
  assert(blocks.front().n_cols == n);
  arma::sp_mat ret(n * blocks.size(), n * blocks.size());
  ret.zeros();
  for (size_t i = 0; i < blocks.size(); i++)
    ret(arma::span(i * n, (i + 1) * n - 1),
        arma::span(i * n, (i + 1) * n - 1)) = blocks[i];
  return ret;
}

static inline SDP
ConstructLogChebychevApproxSdp(const arma::mat& A, const arma::vec& b)
{
  if (A.n_rows != b.n_elem)
    Log::Fatal << "A.n_rows != len(b)" << std::endl;
  const size_t p = A.n_rows;
  const size_t k = A.n_cols;

  // [0, 0, 0]
  // [0, 0, 1]
  // [0, 1, 0]
  arma::sp_mat cblock(3, 3);
  cblock(1, 2) = cblock(2, 1) = 1.;
  const arma::sp_mat C = RepeatBlockDiag(cblock, p);

  SDP sdp(C.n_rows, k + 1, 0);
  sdp.SparseC() = C;
  sdp.SparseB().zeros();
  sdp.SparseB()[0] = -1;

  // [1, 0, 0]
  // [0, 0, 0]
  // [0, 0, 1]
  arma::sp_mat a0block(3, 3);
  a0block(0, 0) = a0block(2, 2) = 1.;
  sdp.SparseA()[0] = RepeatBlockDiag(a0block, p);
  sdp.SparseA()[0] *= -1.;

  for (size_t i = 0; i < k; i++)
  {
    std::vector<arma::sp_mat> blocks;
    for (size_t j = 0; j < p; j++)
    {
      arma::sp_mat block(3, 3);
      const double f = A(j, i) / b(j);
      // [ -a_j(i)/b_j     0        0 ]
      // [      0       a_j(i)/b_j  0 ]
      // [      0          0        0 ]
      block(0, 0) = -f;
      block(1, 1) = f;
      blocks.emplace_back(block);
    }
    sdp.SparseA()[i + 1] = BlockDiag(blocks);
    sdp.SparseA()[i + 1] *= -1;
  }

  return sdp;
}

static inline arma::mat
RandomOrthogonalMatrix(size_t rows, size_t cols)
{
  arma::mat Q, R;
  if (!arma::qr(Q, R, arma::randu<arma::mat>(rows, cols)))
    Log::Fatal << "could not compute QR decomposition" << std::endl;
  return Q;
}

static inline arma::mat
RandomFullRowRankMatrix(size_t rows, size_t cols)
{
  const arma::mat U = RandomOrthogonalMatrix(rows, rows);
  const arma::mat V = RandomOrthogonalMatrix(cols, cols);
  arma::mat S;
  S.zeros(rows, cols);
  for (size_t i = 0; i < std::min(rows, cols); i++)
  {
    S(i, i) = math::Random() + 1e-3;
  }
  return U * S * V;
}

/**
 * See the examples section, Eq. 9, of
 *
 *   Semidefinite Programming.
 *   Lieven Vandenberghe and Stephen Boyd.
 *   SIAM Review. 1996.
 *
 * The logarithmic Chebychev approximation to Ax = b, A is p x k and b is
 * length p is given by the SDP:
 *
 *   min    t
 *   s.t.
 *          [ t - dot(a_i, x)          0             0 ]
 *          [       0           dot(a_i, x) / b_i    1 ]  >= 0, i=1,...,p
 *          [       0                  1             t ]
 *
 */
BOOST_AUTO_TEST_CASE(LogChebychevApproxSdp)
{
  const size_t p0 = 5;
  const size_t k0 = 10;
  const arma::mat A0 = RandomFullRowRankMatrix(p0, k0);
  const arma::vec b0 = arma::randu<arma::vec>(p0);
  const SDP sdp0 = ConstructLogChebychevApproxSdp(A0, b0);
  PrimalDualSolver solver0(sdp0);
  arma::mat X0, Z0;
  arma::vec ysparse0, ydense0;
  const auto stat0 = solver0.Optimize(X0, ysparse0, ydense0, Z0);
  BOOST_REQUIRE(stat0.first);

  const size_t p1 = 10;
  const size_t k1 = 5;
  const arma::mat A1 = RandomFullRowRankMatrix(p1, k1);
  const arma::vec b1 = arma::randu<arma::vec>(p1);
  const SDP sdp1 = ConstructLogChebychevApproxSdp(A1, b1);
  PrimalDualSolver solver1(sdp1);
  arma::mat X1, Z1;
  arma::vec ysparse1, ydense1;
  const auto stat1 = solver1.Optimize(X1, ysparse1, ydense1, Z1);
  BOOST_REQUIRE(stat1.first);
}

BOOST_AUTO_TEST_SUITE_END();
