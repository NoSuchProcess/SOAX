/**
 * Copyright (c) 2015, Lehigh University
 * All rights reserved.
 * See COPYING for license.
 *
 * This file implements the solvers for linear system for SOAX.
 */

#include "./solver_bank.h"

namespace soax {

SolverBank::SolverBank() : alpha_(0.01), beta_(0.1), gamma_(2.0) {}

SolverBank::~SolverBank() {
  this->ClearSolvers(open_solvers_);
  this->ClearSolvers(closed_solvers_);
}

void SolverBank::ClearSolvers(SolverContainer &solvers) {
  if (solvers.empty()) return;
  for (SolverContainer::iterator it = solvers.begin();
       it != solvers.end(); ++it) {
    if (*it) {
      (*it)->DestroyMatrix(0);
      (*it)->DestroyVector(0);
      (*it)->DestroySolution(0);
      delete *it;
    }
  }
  solvers.clear();
}

void SolverBank::Reset(bool reset_matrix) {
  if (reset_matrix) {
    this->ClearSolvers(open_solvers_);
    this->ClearSolvers(closed_solvers_);
  } else {
    this->ResetSolutionAndVector(open_solvers_);
    this->ResetSolutionAndVector(closed_solvers_);
  }
}

void SolverBank::ResetSolutionAndVector(SolverContainer &solvers) {
  if (solvers.empty()) return;
  for (SolverContainer::iterator it = solvers.begin();
       it != solvers.end(); ++it) {
    if (*it) {
      (*it)->InitializeSolution(0);
      (*it)->InitializeVector(0);
    }
  }
}

void SolverBank::SolveSystem(const VectorContainer &vectors, unsigned dim,
                             bool open) {
  SolverContainer &solvers = open ? open_solvers_ : closed_solvers_;
  unsigned position = vectors.size() - kMinimumEvolvingSize;

  if (position >= solvers.size()) {
    this->ExpandSolverContainer(solvers, position);
  }

  if (!solvers[position]) {
    solvers[position] = new SolverType;
    this->InitializeSolver(solvers[position], vectors.size(), open);
  }

  for (unsigned i = 0; i < vectors.size(); ++i) {
    solvers[position]->SetVectorValue(i, vectors[i][dim], 0);
  }
  solvers[position]->Solve();
}

void SolverBank::ExpandSolverContainer(SolverContainer &solvers,
                                       unsigned position) {
  unsigned num_added_solvers = position - solvers.size() + 1;
  for (unsigned i = 0; i < num_added_solvers; ++i) {
    solvers.push_back(NULL);
  }
}

void SolverBank::InitializeSolver(SolverType *solver, unsigned order,
                                  bool open) {
  solver->SetNumberOfMatrices(1);
  solver->SetNumberOfVectors(1);
  solver->SetNumberOfSolutions(1);
  solver->SetMaximumNonZeroValuesInMatrix(kMinimumEvolvingSize * order);
  solver->SetSystemOrder(order);

  solver->InitializeMatrix(0);
  solver->InitializeVector(0);
  solver->InitializeSolution(0);

  if (open)
    this->FillMatrixOpen(solver, order);
  else
    this->FillMatrixClosed(solver, order);
}

void SolverBank::FillMatrixOpen(SolverType *solver, unsigned order) {
  /* alpha_0 = 0; alpha_{N-1} = alpha; beta_0 = beta_{N-1} = 0 */
  const double diag0 = 2 * alpha_ + 6 * beta_ + gamma_;
  const double diag1 = -alpha_ - 4 * beta_;

  // main diagonal
  solver->SetMatrixValue(0, 0, alpha_ + beta_ + gamma_, 0);
  solver->SetMatrixValue(1, 1, 2 * alpha_ + 5 * beta_ + gamma_, 0);
  for (unsigned i = 2; i < order - 2; i++)
    solver->SetMatrixValue(i, i, diag0, 0);
  solver->SetMatrixValue(order - 2, order - 2,
                         2 * alpha_ + 5 * beta_ + gamma_, 0);
  solver->SetMatrixValue(order - 1, order - 1, alpha_ + beta_ + gamma_, 0);

  // +1/-1 diagonal
  solver->SetMatrixValue(0, 1, -alpha_ - 2 * beta_, 0);
  solver->SetMatrixValue(1, 0, -alpha_ - 2 * beta_, 0);
  for (unsigned i = 1; i < order - 2; i++) {
    solver->SetMatrixValue(i, i + 1, diag1, 0);
    solver->SetMatrixValue(i + 1, i, diag1, 0);
  }
  solver->SetMatrixValue(order - 2, order - 1, -alpha_ - 2 * beta_, 0);
  solver->SetMatrixValue(order - 1, order - 2, -alpha_ - 2 * beta_, 0);

  // +2/-2 diagonal
  for (unsigned i = 2; i < order; ++i) {
    solver->SetMatrixValue(i, i-2, beta_, 0);
    solver->SetMatrixValue(i-2, i, beta_, 0);
  }
}

void SolverBank::FillMatrixClosed(SolverType *solver, unsigned order) {
  const double diag0 = 2 * alpha_ + 6 * beta_ + gamma_;
  const double diag1 = -alpha_ - 4 * beta_;

  for (unsigned i = 0; i < order; ++i) {
    solver->SetMatrixValue(i, (i+order-2)%order, beta_, 0);
    solver->SetMatrixValue(i, (i+order-1)%order, diag1, 0);
    solver->SetMatrixValue(i, i, diag0, 0);
    solver->SetMatrixValue(i, (i+order+1)%order, diag1, 0);
    solver->SetMatrixValue(i, (i+order+2)%order, beta_, 0);
  }
}

double SolverBank::GetSolution(unsigned order, unsigned index, bool open) {
  SolverContainer &solvers = open? open_solvers_ : closed_solvers_;
  return solvers[order - kMinimumEvolvingSize]->GetSolutionValue(index, 0);
}

}  // namespace soax
