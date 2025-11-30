#include "cut_to_disk.h"

#include <map>
#include <set>
#include <deque>
#include <algorithm>

namespace igl {
  template <typename DerivedF, typename Index>
  void cut_to_disk(
    const Eigen::MatrixBase<DerivedF> &F,
    std::vector<std::vector<Index> > &cuts)
  {
    cuts.clear();

    Index nfaces = F.rows();

    if (nfaces == 0)
        return;

    std::map<std::pair<Index, Index>, std::vector<Index> > edges;
    // build edges

    for (Index i = 0; i < nfaces; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            Index v0 = F(i, j);
            Index v1 = F(i, (j + 1) % 3);
            std::pair<Index, Index> e;
            e.first = std::min(v0, v1);
            e.second = std::max(v0, v1);
            edges[e].push_back(i);
        }
    }

    int nedges = edges.size();
    Eigen::Matrix<Index, -1, -1> edgeVerts(nedges,2);
    Eigen::Matrix<Index, -1, -1> edgeFaces(nedges,2);
    Eigen::Matrix<Index, -1, -1> faceEdges(nfaces, 3);
    std::set<Index> boundaryEdges;
    std::map<std::pair<Index, Index>, Index> edgeidx;
    Index idx = 0;
    for (auto it : edges)
    {
        edgeidx[it.first] = idx;
        edgeVerts(idx, 0) = it.first.first;
        edgeVerts(idx, 1) = it.first.second;
        edgeFaces(idx, 0) = it.second[0];
        if (it.second.size() > 1)
        {
            edgeFaces(idx, 1) = it.second[1];
        }
        else
        {
            edgeFaces(idx, 1) = -1;
            boundaryEdges.insert(idx);
        }
        idx++;
    }
    for (Index i = 0; i < nfaces; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            Index v0 = F(i, j);
            Index v1 = F(i, (j + 1) % 3);
            std::pair<Index, Index> e;
            e.first = std::min(v0, v1);
            e.second = std::max(v0, v1);
            faceEdges(i, j) = edgeidx[e];
        }
    }

    bool *deleted = new bool[nfaces];
    for (Index i = 0; i < nfaces; i++)
        deleted[i] = false;

    std::set<Index> deletededges;

    // loop over faces
    for (Index face = 0; face < nfaces; face++)
    {
        // stop at first undeleted face
        if (deleted[face])
            continue;
        deleted[face] = true;
        std::deque<Index> processEdges;
        for (int i = 0; i < 3; i++)
        {
            Index e = faceEdges(face, i);
            if (boundaryEdges.count(e))
                continue;
            int ndeleted = 0;
            if (deleted[edgeFaces(e, 0)])
                ndeleted++;
            if (deleted[edgeFaces(e, 1)])
                ndeleted++;
            if (ndeleted == 1)
                processEdges.push_back(e);
        }
        // delete all faces adjacent to edges with exactly one adjacent face
        while (!processEdges.empty())
        {
            Index nexte = processEdges.front();
            processEdges.pop_front();
            Index todelete = nfaces;
            if (!deleted[edgeFaces(nexte, 0)])
                todelete = edgeFaces(nexte, 0);
            if (!deleted[edgeFaces(nexte, 1)])
                todelete = edgeFaces(nexte, 1);
            if (todelete != nfaces)
            {
                deletededges.insert(nexte);
                deleted[todelete] = true;
                for (int i = 0; i < 3; i++)
                {
                    Index e = faceEdges(todelete, i);
                    if (boundaryEdges.count(e))
                        continue;
                    int ndeleted = 0;
                    if (deleted[edgeFaces(e, 0)])
                        ndeleted++;
                    if (deleted[edgeFaces(e, 1)])
                        ndeleted++;
                    if (ndeleted == 1)
                        processEdges.push_back(e);
                }
            }
        }
    }
    delete[] deleted;

    // accumulated non-deleted edges
    std::vector<Index> leftedges;
    for (Index i = 0; i < nedges; i++)
    {
        if (!deletededges.count(i))
            leftedges.push_back(i);
    }

    deletededges.clear();
    // prune spines
    std::map<Index, std::vector<Index> > spinevertedges;
    for (Index i : leftedges)
    {
        spinevertedges[edgeVerts(i, 0)].push_back(i);
        spinevertedges[edgeVerts(i, 1)].push_back(i);
    }

    std::deque<Index> vertsProcess;
    std::map<Index, int> spinevertnbs;
    for (auto it : spinevertedges)
    {
        spinevertnbs[it.first] = it.second.size();
        if (it.second.size() == 1)
            vertsProcess.push_back(it.first);
    }
    while (!vertsProcess.empty())
    {
        Index vert = vertsProcess.front();
        vertsProcess.pop_front();
        for (Index e : spinevertedges[vert])
        {
            if (!deletededges.count(e))
            {
                deletededges.insert(e);
                for (int j = 0; j < 2; j++)
                {
                    spinevertnbs[edgeVerts(e, j)]--;
                    if (spinevertnbs[edgeVerts(e, j)] == 1)
                    {
                        vertsProcess.push_back(edgeVerts(e, j));
                    }
                }
            }
        }
    }
    std::vector<Index> loopedges;
    for (Index i : leftedges)
        if (!deletededges.count(i))
            loopedges.push_back(i);

    Index nloopedges = loopedges.size();
    if (nloopedges == 0)
        return;

    std::map<Index, std::vector<Index> > loopvertedges;
    for (Index e : loopedges)
    {
        loopvertedges[edgeVerts(e, 0)].push_back(e);
        loopvertedges[edgeVerts(e, 1)].push_back(e);
    }

    std::set<Index> usededges;
    for (Index e : loopedges)
    {
        // make a cycle or chain starting from this edge
        while (!usededges.count(e))
        {
            std::vector<Index> cycleverts;
            std::vector<Index> cycleedges;
            cycleverts.push_back(edgeVerts(e, 0));
            cycleverts.push_back(edgeVerts(e, 1));
            cycleedges.push_back(e);

            std::map<Index, Index> cycleidx;
            cycleidx[cycleverts[0]] = 0;
            cycleidx[cycleverts[1]] = 1;

            Index curvert = edgeVerts(e, 1);
            Index cure = e;
            bool foundcycle = false;
            while (curvert != -1 && !foundcycle)
            {
                Index nextvert = -1;
                Index nexte = -1;
                for (Index cande : loopvertedges[curvert])
                {
                    if (!usededges.count(cande) && cande != cure)
                    {
                        int vidx = 0;
                        if (curvert == edgeVerts(cande, vidx))
                            vidx = 1;
                        nextvert = edgeVerts(cande, vidx);
                        nexte = cande;
                        break;
                    }
                }
                if (nextvert != -1)
                {
                    auto it = cycleidx.find(nextvert);
                    if (it != cycleidx.end())
                    {
                        // we've hit outselves
                        std::vector<Index> cut;
                        for (Index i = it->second; i < cycleverts.size(); i++)
                        {
                            cut.push_back(cycleverts[i]);
                        }
                        cut.push_back(nextvert);
                        cuts.push_back(cut);
                        for (Index i = it->second; i < cycleedges.size(); i++)
                        {
                            usededges.insert(cycleedges[i]);
                        }
                        usededges.insert(nexte);
                        foundcycle = true;
                    }
                    else
                    {
                        cycleidx[nextvert] = cycleverts.size();
                        cycleverts.push_back(nextvert);
                        cycleedges.push_back(nexte);
                    }
                }
                curvert = nextvert;
                cure = nexte;
            }
            if (!foundcycle)
            {
                // we've hit a dead end. reverse and try the other direction
                std::reverse(cycleverts.begin(), cycleverts.end());
                std::reverse(cycleedges.begin(), cycleedges.end());
                cycleidx.clear();
                for (Index i = 0; i < cycleverts.size(); i++)
                {
                    cycleidx[cycleverts[i]] = i;
                }
                
                curvert = cycleverts.back();
                cure = cycleedges.back();
                while (curvert != -1 && !foundcycle)
                {
                    Index nextvert = -1;
                    Index nexte = -1;
                    for (Index cande : loopvertedges[curvert])
                    {
                        if (!usededges.count(cande) && cande != cure)
                        {
                            int vidx = 0;
                            if (curvert == edgeVerts(cande, vidx))
                                vidx = 1;
                            nextvert = edgeVerts(cande, vidx);
                            nexte = cande;
                            break;
                        }
                    }
                    if (nextvert != -1)
                    {
                        auto it = cycleidx.find(nextvert);
                        if (it != cycleidx.end())
                        {
                            // we've hit outselves
                            std::vector<Index> cut;
                            for (Index i = it->second; i < cycleverts.size(); i++)
                            {
                                cut.push_back(cycleverts[i]);
                            }
                            cut.push_back(nextvert);
                            cuts.push_back(cut);
                            for (Index i = it->second; i < cycleedges.size(); i++)
                            {
                                usededges.insert(cycleedges[i]);
                            }
                            usededges.insert(nexte);
                            foundcycle = true;
                        }
                        else
                        {
                            cycleidx[nextvert] = cycleverts.size();
                            cycleverts.push_back(nextvert);
                            cycleedges.push_back(nexte);
                        }
                    }
                    curvert = nextvert;
                    cure = nexte;
                }
                if (!foundcycle)
                {
                    // we've found a chain
                    std::vector<Index> cut;
                    for (Index i = 0; i < cycleverts.size(); i++)
                    {
                        cut.push_back(cycleverts[i]);
                    }
                    cuts.push_back(cut);
                    for (Index i = 0; i < cycleedges.size(); i++)
                    {
                        usededges.insert(cycleedges[i]);
                    }
                }
            }
        }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::cut_to_disk<Eigen::Matrix<int, -1, -1, 0, -1, -1>, int>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&);

#endif
