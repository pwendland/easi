/**
 * @file
 * This file is part of SeisSol.
 *
 * @author Carsten Uphoff (c.uphoff AT tum.de, http://www5.in.tum.de/wiki/index.php/Carsten_Uphoff,_M.Sc.)
 *
 * @section LICENSE
 * Copyright (c) 2017, SeisSol Group
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * @section DESCRIPTION
 **/
#ifndef EASI_COMPONENT_COMPOSITE_H_
#define EASI_COMPONENT_COMPOSITE_H_

#include <vector>
#include <algorithm>
#include <numeric>
#include <sstream>
#include "easi/Component.h"
#include "easi/util/Print.h"

namespace easi {
class Composite : public Component {
public:
  typedef typename std::vector<Component*>::iterator iterator;
  typedef typename std::vector<Component*>::const_iterator const_iterator;

  virtual ~Composite();
  
  virtual void add(Component* component);

  virtual iterator begin() { return m_components.begin(); }
  virtual const_iterator cbegin() const { return m_components.cbegin(); }
  
  virtual iterator end() { return m_components.end(); }
  virtual const_iterator cend() const { return m_components.cend(); }
  
  virtual void evaluate(Query& query, ResultAdapter& result);
  
  std::string addMissingParameters(std::string const& what_arg, std::set<std::string> expected, std::set<std::string> supplied);

  virtual std::set<std::string> suppliedParameters();
  
protected:
  virtual Matrix<double> map(Matrix<double>& x) = 0;
  
private:
  void couldNotFindModelError(int group, Slice<double> const& y);
  std::vector<Component*> m_components;
};

Composite::~Composite() {
  std::for_each(begin(), end(), [](Component*& component) { delete component; });
}

void Composite::add(Component* component) {
  if (out() != component->in()) {
    std::ostringstream os;
    os << "The input parameters of a component (";
    printWithSeparator(component->in(), os);
    os << ") added to a composite are incompatible (should be ";
    printWithSeparator(out(), os);
    os << ").";
    throw std::invalid_argument(addFileReference(os.str()));
  }
  
  m_components.push_back(component);
}

void Composite::evaluate(Query& query, ResultAdapter& result) {
  // Map dimensions
  Matrix<double> y = map(query.x);
  
  // Find model for each point
  unsigned nComponents = m_components.size();
  if (nComponents == 0) {
    if (!result.isSubset(out())) {
      throw std::invalid_argument(
        addFileReference(
          addMissingParameters("Component does not supply all required parameters.", result.parameters(), out())
        )
      );
    }
    unsigned col = 0;
    for (std::string const& param : out()) {
      result.set(param, query.index, y.colSlice(col));
      ++col;
    }
  } else if (nComponents == 1) {
    if (query.numPoints() > 0) {
      if (!m_components[0]->acceptAlways()) {
        for (unsigned i = 0; i < query.numPoints(); ++i) {
          if (!m_components[0]->accept(query.group(i), y.rowSlice(i))) {
            couldNotFindModelError(query.group(i), y.rowSlice(i));
          }
        }
      }
      query.x = y;
      m_components[0]->evaluate(query, result);
    }
  } else {
    unsigned* model = new unsigned[query.numPoints() + nComponents];
    unsigned* modelCount = model + query.numPoints();
    std::for_each(modelCount, modelCount + nComponents, [](unsigned& x) { x = 0; });
    for (unsigned i = 0; i < query.numPoints(); ++i) {
      unsigned c = 0;
      for (; c < nComponents; ++c) {
        if (m_components[c]->accept(query.group(i), y.rowSlice(i))) {
          model[i] = c;
          ++modelCount[c];
          break;
        }
      }
      if (c == nComponents) {
        couldNotFindModelError(query.group(i), y.rowSlice(i));
      }
    }
    
    // Allocate queries
    void* queriesMemory = operator new[](nComponents * sizeof(Query));
    Query* queries = static_cast<Query*>(queriesMemory);
    for (unsigned c = 0; c < nComponents; ++c) {
      new(&queries[c]) Query(modelCount[c], dimCodomain(), false);
    }
    
    // Copy points to queries
    std::for_each(modelCount, modelCount + nComponents, [](unsigned& x) { x = 0; });
    for (unsigned i = 0; i < query.numPoints(); ++i) {
      Query& target =    queries[ model[i] ];
      unsigned& mc  = modelCount[ model[i] ];

      target.group(mc) = query.group(i);
      for (unsigned d = 0; d < dimCodomain(); ++d) {
        target.x(mc,d) = y(i,d);
      }
      target.index(mc) = query.index(i);
      ++mc;
    }
    
    // Cleanup query and temporary data
    delete[] model;
    y.clear();
    query.clear();
    
    // Evaluate submodels
    for (unsigned c = 0; c < nComponents; ++c) {
      if (queries[c].numPoints() > 0) {
        m_components[c]->evaluate(queries[c], result);
      }
    }
    
    // Delete queries
    for (unsigned c = 0; c < nComponents; ++c) {
      queries[c].~Query();
    }
    operator delete[](queriesMemory);
  }
}

void Composite::couldNotFindModelError(int group, Slice<double> const& y) {
  std::stringstream ss;
  ss << "Could not find model for point [ ";
  for (unsigned d = 0; d < y.size(); ++d) {
    ss << y(d) << " ";
  }
  ss << "] in group " << group << ".";
  throw std::runtime_error(addFileReference(ss.str()));
}

std::string Composite::addMissingParameters(std::string const& what_arg, std::set<std::string> expected, std::set<std::string> supplied) {
  std::set<std::string> missing;
  std::set_difference(  expected.begin(), expected.end(),
                        supplied.begin(), supplied.end(),
                        std::inserter(missing, missing.begin()));
  
  std::stringstream s;
  s << what_arg << " Missing: {";
  printWithSeparator(missing, s);
  s << "}.";
  return s.str();
}

std::set<std::string> Composite::suppliedParameters() {
  if (m_components.empty()) {
    return Component::suppliedParameters();
  }

  return std::accumulate(begin(), end(), std::set<std::string>(), [](std::set<std::string>& a, Component*& b) {
    std::set<std::string> supplied;
    auto c = b->suppliedParameters();
    std::set_union( a.begin(), a.end(),
                    c.begin(), c.end(),
                    std::inserter(supplied, supplied.begin()) );
    return supplied;
  });
}
}

#endif
