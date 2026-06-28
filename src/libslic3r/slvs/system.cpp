//-----------------------------------------------------------------------------
// Once we've written our constraint equations in the symbolic algebra system,
// these routines linearize them, and solve by a modified Newton's method.
// This also contains the routines to detect non-convergence or inconsistency,
// and report diagnostics to the user.
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------
#include "solvespace.h"

// This tolerance is used to determine whether two (linearized) constraints
// are linearly dependent. If this is too small, then we will attempt to
// solve truly inconsistent systems and fail. But if it's too large, then
// we will give up on legitimate systems like a skinny right angle triangle by
// its hypotenuse and long side.
const double System::RANK_MAG_TOLERANCE = 1e-4;

// The solver will converge all unknowns to within this tolerance. This must
// always be much less than LENGTH_EPS, and in practice should be much less.
const double System::CONVERGE_TOLERANCE = (LENGTH_EPS/(1e2));

bool System::WriteJacobian(int tag) {
    int a, i, j;

    j = 0;
    for(a = 0; a < param.n; a++) {
        if(j >= MAX_UNKNOWNS) return false;

        Param *p = &(param.elem[a]);
        if(p->tag != tag) continue;
        mat.param[j] = p->h;
        j++;
    }
    mat.n = j;

    i = 0;
    for(a = 0; a < eq.n; a++) {
        if(i >= MAX_UNKNOWNS) return false;

        Equation *e = &(eq.elem[a]);
        if(e->tag != tag) continue;

        mat.eq[i] = e->h;
        Expr *f = e->e->DeepCopyWithParamsAsPointers(&param, &(SK.param));
        f = f->FoldConstants();

        // Hash table (61 bits) to accelerate generation of zero partials.
        uint64_t scoreboard = f->ParamsUsed();
        for(j = 0; j < mat.n; j++) {
            Expr *pd;
            if(scoreboard & ((uint64_t)1 << (mat.param[j].v % 61)) &&
                f->DependsOn(mat.param[j]))
            {
                pd = f->PartialWrt(mat.param[j]);
                pd = pd->FoldConstants();
                pd = pd->DeepCopyWithParamsAsPointers(&param, &(SK.param));
            } else {
                pd = Expr::From(0.0);
            }
            mat.A.sym[i][j] = pd;
        }
        mat.B.sym[i] = f;
        i++;
    }
    mat.m = i;

    return true;
}

void System::EvalJacobian() {
    int i, j;
    for(i = 0; i < mat.m; i++) {
        for(j = 0; j < mat.n; j++) {
            mat.A.num[i][j] = (mat.A.sym[i][j])->Eval();
        }
    }
}

bool System::IsDragged(hParam p) {
    hParam *pp;
    for(pp = dragged.First(); pp; pp = dragged.NextAfter(pp)) {
        if(p.v == pp->v) return true;
    }
    return false;
}

void System::SolveBySubstitution() {
    int i;
    for(i = 0; i < eq.n; i++) {
        Equation *teq = &(eq.elem[i]);
        Expr *tex = teq->e;

        if(tex->op    == Expr::Op::MINUS &&
           tex->a->op == Expr::Op::PARAM &&
           tex->b->op == Expr::Op::PARAM)
        {
            hParam a = tex->a->parh;
            hParam b = tex->b->parh;
            if(!(param.FindByIdNoOops(a) && param.FindByIdNoOops(b))) {
                // Don't substitute unless they're both solver params;
                // otherwise it's an equation that can be solved immediately,
                // or an error to flag later.
                continue;
            }

            if(IsDragged(a)) {
                // A is being dragged, so A should stay, and B should go
                hParam t = a;
                a = b;
                b = t;
            }

            int j;
            for(j = 0; j < eq.n; j++) {
                Equation *req = &(eq.elem[j]);
                (req->e)->Substitute(a, b); // A becomes B, B unchanged
            }
            for(j = 0; j < param.n; j++) {
                Param *rp = &(param.elem[j]);
                if(rp->substd.v == a.v) {
                    rp->substd = b;
                }
            }
            Param *ptr = param.FindById(a);
            ptr->tag = VAR_SUBSTITUTED;
            ptr->substd = b;

            teq->tag = EQ_SUBSTITUTED;
        }
    }
}

//-----------------------------------------------------------------------------
// Calculate the rank of the Jacobian matrix, by Gram-Schimdt orthogonalization
// in place. A row (~equation) is considered to be all zeros if its magnitude
// is less than the tolerance RANK_MAG_TOLERANCE.
//-----------------------------------------------------------------------------
int System::CalculateRank() {
    // Actually work with magnitudes squared, not the magnitudes
    double rowMag[MAX_UNKNOWNS] = {};
    double tol = RANK_MAG_TOLERANCE*RANK_MAG_TOLERANCE;

    int i, iprev, j;
    int rank = 0;

    for(i = 0; i < mat.m; i++) {
        // Subtract off this row's component in the direction of any
        // previous rows
        for(iprev = 0; iprev < i; iprev++) {
            if(rowMag[iprev] <= tol) continue; // ignore zero rows

            double dot = 0;
            for(j = 0; j < mat.n; j++) {
                dot += (mat.A.num[iprev][j]) * (mat.A.num[i][j]);
            }
            for(j = 0; j < mat.n; j++) {
                mat.A.num[i][j] -= (dot/rowMag[iprev])*mat.A.num[iprev][j];
            }
        }
        // Our row is now normal to all previous rows; calculate the
        // magnitude of what's left
        double mag = 0;
        for(j = 0; j < mat.n; j++) {
            mag += (mat.A.num[i][j]) * (mat.A.num[i][j]);
        }
        if(mag > tol) {
            rank++;
        }
        rowMag[i] = mag;
    }

    return rank;
}

bool System::TestRank() {
    EvalJacobian();
    return CalculateRank() == mat.m;
}

bool System::SolveLinearSystem(double X[], double A[][MAX_UNKNOWNS],
                               double B[], int n)
{
    // Gaussian elimination, with partial pivoting. It's an error if the
    // matrix is singular, because that means two constraints are
    // equivalent.
    int i, j, ip, jp, imax = 0;
    double max, temp;

    for(i = 0; i < n; i++) {
        // We are trying eliminate the term in column i, for rows i+1 and
        // greater. First, find a pivot (between rows i and N-1).
        max = 0;
        for(ip = i; ip < n; ip++) {
            if(ffabs(A[ip][i]) > max) {
                imax = ip;
                max = ffabs(A[ip][i]);
            }
        }
        // Don't give up on a singular matrix unless it's really bad; the
        // assumption code is responsible for identifying that condition,
        // so we're not responsible for reporting that error.
        if(ffabs(max) < 1e-20) continue;

        // Swap row imax with row i
        for(jp = 0; jp < n; jp++) {
            swap(A[i][jp], A[imax][jp]);
        }
        swap(B[i], B[imax]);

        // For rows i+1 and greater, eliminate the term in column i.
        for(ip = i+1; ip < n; ip++) {
            temp = A[ip][i]/A[i][i];

            for(jp = i; jp < n; jp++) {
                A[ip][jp] -= temp*(A[i][jp]);
            }
            B[ip] -= temp*B[i];
        }
    }

    // We've put the matrix in upper triangular form, so at this point we
    // can solve by back-substitution.
    for(i = n - 1; i >= 0; i--) {
        if(ffabs(A[i][i]) < 1e-20) continue;

        temp = B[i];
        for(j = n - 1; j > i; j--) {
            temp -= X[j]*A[i][j];
        }
        X[i] = temp / A[i][i];
    }

    return true;
}

bool System::SolveLeastSquares() {
    int r, c, i;

    // Scale the columns; this scale weights the parameters for the least
    // squares solve, so that we can encourage the solver to make bigger
    // changes in some parameters, and smaller in others.
    for(c = 0; c < mat.n; c++) {
        if(IsDragged(mat.param[c])) {
            // It's least squares, so this parameter doesn't need to be all
            // that big to get a large effect.
            mat.scale[c] = 1/20.0;
        } else {
            mat.scale[c] = 1;
        }
        for(r = 0; r < mat.m; r++) {
            mat.A.num[r][c] *= mat.scale[c];
        }
    }

    // Write A*A'
    for(r = 0; r < mat.m; r++) {
        for(c = 0; c < mat.m; c++) {  // yes, AAt is square
            double sum = 0;
            for(i = 0; i < mat.n; i++) {
                sum += mat.A.num[r][i]*mat.A.num[c][i];
            }
            mat.AAt[r][c] = sum;
        }
    }

    if(!SolveLinearSystem(mat.Z, mat.AAt, mat.B.num, mat.m)) return false;

    // And multiply that by A' to get our solution.
    for(c = 0; c < mat.n; c++) {
        double sum = 0;
        for(i = 0; i < mat.m; i++) {
            sum += mat.A.num[i][c]*mat.Z[i];
        }
        mat.X[c] = sum * mat.scale[c];
    }
    return true;
}

bool System::NewtonSolve(int tag) {

    int iter = 0;
    bool converged = false;
    int i;

    // Evaluate the functions at our operating point.
    for(i = 0; i < mat.m; i++) {
        mat.B.num[i] = (mat.B.sym[i])->Eval();
    }
    do {
        // And evaluate the Jacobian at our initial operating point.
        EvalJacobian();

        if(!SolveLeastSquares()) break;

        // Take the Newton step;
        //      J(x_n) (x_{n+1} - x_n) = 0 - F(x_n)
        for(i = 0; i < mat.n; i++) {
            Param *p = param.FindById(mat.param[i]);
            p->val -= mat.X[i];
            if(isnan(p->val)) {
                // Very bad, and clearly not convergent
                return false;
            }
        }

        // Re-evalute the functions, since the params have just changed.
        for(i = 0; i < mat.m; i++) {
            mat.B.num[i] = (mat.B.sym[i])->Eval();
        }
        // Check for convergence
        converged = true;
        for(i = 0; i < mat.m; i++) {
            if(isnan(mat.B.num[i])) {
                return false;
            }
            if(ffabs(mat.B.num[i]) > CONVERGE_TOLERANCE) {
                converged = false;
                break;
            }
        }
    } while(iter++ < 50 && !converged);

    return converged;
}

void System::WriteEquationsExceptFor(hConstraint hc, Group *g) {
    int i;
    // Generate all the equations from constraints in this group
    for(i = 0; i < SK.constraint.n; i++) {
        ConstraintBase *c = &(SK.constraint.elem[i]);
        if(c->group.v != g->h.v) continue;
        if(c->h.v == hc.v) continue;

        if(c->HasLabel() && c->type != Constraint::Type::COMMENT &&
                g->allDimsReference)
        {
            // When all dimensions are reference, we adjust them to display
            // the correct value, and then don't generate any equations.
            c->ModifyToSatisfy();
            continue;
        }
        if(g->relaxConstraints && c->type != Constraint::Type::POINTS_COINCIDENT) {
            // When the constraints are relaxed, we keep only the point-
            // coincident constraints, and the constraints generated by
            // the entities and groups.
            continue;
        }

        c->GenerateEquations(&eq);
    }
    // And the equations from entities
    for(i = 0; i < SK.entity.n; i++) {
        EntityBase *e = &(SK.entity.elem[i]);
        if(e->group.v != g->h.v) continue;

        e->GenerateEquations(&eq);
    }
    // And from the groups themselves
    g->GenerateEquations(&eq);
}

void System::FindWhichToRemoveToFixJacobian(Group *g, List<hConstraint> *bad, bool forceDofCheck) {
    int a, i;

    for(a = 0; a < 2; a++) {
        for(i = 0; i < SK.constraint.n; i++) {
            ConstraintBase *c = &(SK.constraint.elem[i]);
            if(c->group.v != g->h.v) continue;
            if((c->type == Constraint::Type::POINTS_COINCIDENT && a == 0) ||
               (c->type != Constraint::Type::POINTS_COINCIDENT && a == 1))
            {
                // Do the constraints in two passes: first everything but
                // the point-coincident constraints, then only those
                // constraints (so they appear last in the list).
                continue;
            }

            param.ClearTags();
            eq.Clear();
            WriteEquationsExceptFor(c->h, g);
            eq.ClearTags();

            // It's a major speedup to solve the easy ones by substitution here,
            // and that doesn't break anything.
            if(!forceDofCheck) {
                SolveBySubstitution();
            }

            WriteJacobian(0);
            EvalJacobian();

            int rank = CalculateRank();
            if(rank == mat.m) {
                // We fixed it by removing this constraint
                bad->Add(&(c->h));
            }
        }
    }
}

SolveResult System::Solve(Group *g, int *dof, List<hConstraint> *bad,
                          bool andFindBad, bool andFindFree, bool forceDofCheck)
{
    WriteEquationsExceptFor(Constraint::NO_CONSTRAINT, g);

    int i;
    bool rankOk;

/*
    dbp("%d equations", eq.n);
    for(i = 0; i < eq.n; i++) {
        dbp("  %.3f = %s = 0", eq.elem[i].e->Eval(), eq.elem[i].e->Print());
    }
    dbp("%d parameters", param.n);
    for(i = 0; i < param.n; i++) {
        dbp("   param %08x at %.3f", param.elem[i].h.v, param.elem[i].val);
    } */

    // All params and equations are assigned to group zero.
    param.ClearTags();
    eq.ClearTags();

    if(!forceDofCheck) {
        SolveBySubstitution();
    }

    // Before solving the big system, see if we can find any equations that
    // are soluble alone. This can be a huge speedup. We don't know whether
    // the system is consistent yet, but if it isn't then we'll catch that
    // later.
    int alone = 1;
    for(i = 0; i < eq.n; i++) {
        Equation *e = &(eq.elem[i]);
        if(e->tag != 0) continue;

        hParam hp = e->e->ReferencedParams(&param);
        if(hp.v == Expr::NO_PARAMS.v) continue;
        if(hp.v == Expr::MULTIPLE_PARAMS.v) continue;

        Param *p = param.FindById(hp);
        if(p->tag != 0) continue; // let rank test catch inconsistency

        e->tag = alone;
        p->tag = alone;
        WriteJacobian(alone);
        if(!NewtonSolve(alone)) {
            // We don't do the rank test, so let's arbitrarily return
            // the DIDNT_CONVERGE result here.
            rankOk = true;
            // Failed to converge, bail out early
            goto didnt_converge;
        }
        alone++;
    }

    // Now write the Jacobian for what's left, and do a rank test; that
    // tells us if the system is inconsistently constrained.
    if(!WriteJacobian(0)) {
        return SolveResult::TOO_MANY_UNKNOWNS;
    }

    rankOk = TestRank();

    // And do the leftovers as one big system
    if(!NewtonSolve(0)) {
        goto didnt_converge;
    }

    rankOk = TestRank();
    if(!rankOk) {
        if(!g->allowRedundant) {
            if(andFindBad) FindWhichToRemoveToFixJacobian(g, bad, forceDofCheck);
        }
    } else {
        // This is not the full Jacobian, but any substitutions or single-eq
        // solves removed one equation and one unknown, therefore no effect
        // on the number of DOF.
        if(dof) *dof = CalculateDof();
        MarkParamsFree(andFindFree);
    }
    // System solved correctly, so write the new values back in to the
    // main parameter table.
    for(i = 0; i < param.n; i++) {
        Param *p = &(param.elem[i]);
        double val;
        if(p->tag == VAR_SUBSTITUTED) {
            val = param.FindById(p->substd)->val;
        } else {
            val = p->val;
        }
        Param *pp = SK.GetParam(p->h);
        pp->val = val;
        pp->known = true;
        pp->free = p->free;
    }
    return rankOk ? SolveResult::OKAY : SolveResult::REDUNDANT_OKAY;

didnt_converge:
    SK.constraint.ClearTags();
    for(i = 0; i < eq.n; i++) {
        if(ffabs(mat.B.num[i]) > CONVERGE_TOLERANCE || isnan(mat.B.num[i])) {
            // This constraint is unsatisfied.
            if(!mat.eq[i].isFromConstraint()) continue;

            hConstraint hc = mat.eq[i].constraint();
            ConstraintBase *c = SK.constraint.FindByIdNoOops(hc);
            if(!c) continue;
            // Don't double-show constraints that generated multiple
            // unsatisfied equations
            if(!c->tag) {
                bad->Add(&(c->h));
                c->tag = 1;
            }
        }
    }

    return rankOk ? SolveResult::DIDNT_CONVERGE : SolveResult::REDUNDANT_DIDNT_CONVERGE;
}

SolveResult System::SolveRank(Group *g, int *dof, List<hConstraint> *bad,
                              bool andFindBad, bool andFindFree, bool forceDofCheck)
{
    WriteEquationsExceptFor(Constraint::NO_CONSTRAINT, g);

    // All params and equations are assigned to group zero.
    param.ClearTags();
    eq.ClearTags();

    if(!forceDofCheck) {
        SolveBySubstitution();
    }

    // Now write the Jacobian, and do a rank test; that
    // tells us if the system is inconsistently constrained.
    if(!WriteJacobian(0)) {
        return SolveResult::TOO_MANY_UNKNOWNS;
    }

    bool rankOk = TestRank();
    if(!rankOk) {
        if(!g->allowRedundant) {
            if(andFindBad) FindWhichToRemoveToFixJacobian(g, bad, forceDofCheck);
        }
    } else {
        // This is not the full Jacobian, but any substitutions or single-eq
        // solves removed one equation and one unknown, therefore no effect
        // on the number of DOF.
        if(dof) *dof = CalculateDof();
        MarkParamsFree(andFindFree);
    }
    return rankOk ? SolveResult::OKAY : SolveResult::REDUNDANT_OKAY;
}

void System::Clear() {
    entity.Clear();
    param.Clear();
    eq.Clear();
    dragged.Clear();
}

void System::MarkParamsFree(bool find) {
    // If requested, find all the free (unbound) variables. This might be
    // more than the number of degrees of freedom. Don't always do this,
    // because the display would get annoying and it's slow.
    for(int i = 0; i < param.n; i++) {
        Param *p = &(param.elem[i]);
        p->free = false;

        if(find) {
            if(p->tag == 0) {
                p->tag = VAR_DOF_TEST;
                WriteJacobian(0);
                EvalJacobian();
                int rank = CalculateRank();
                if(rank == mat.m) {
                    p->free = true;
                }
                p->tag = 0;
            }
        }
    }
}

int System::CalculateDof() {
    return mat.n - mat.m;
}

