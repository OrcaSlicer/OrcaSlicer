#pragma once

#include "slvs.h"

#include <valarray>
#include <vector>
#include <tuple>

class DLL SolveSpaceSystem
{
public:
    SolveSpaceSystem();

    Slvs_hParam addParam(Slvs_Param parameter);

    Slvs_hEntity addEntity(Slvs_Entity entity);

    Slvs_hConstraint addConstr(Slvs_Constraint constr);

    enum ResultStatus {
        RESULT_OKAY              = SLVS_RESULT_OKAY             ,
        RESULT_INCONSISTENT      = SLVS_RESULT_INCONSISTENT     ,
        RESULT_DIDNT_CONVERGE    = SLVS_RESULT_DIDNT_CONVERGE   ,
        RESULT_TOO_MANY_UNKNOWNS = SLVS_RESULT_TOO_MANY_UNKNOWNS,
    };

    ResultStatus solve(Slvs_hGroup groupId, bool reportFailedConstraints = true);

    double parameterValue(Slvs_hParam paramId);
    void   setParameterValue(Slvs_hParam paramId, double value);

    std::tuple< std::valarray<double>,
        std::valarray<double>,
        std::valarray<double> >
        orientationMx(Slvs_hEntity normalIn3dEntityId);

    // Returns point as x, y, z values
    std::valarray<double> global3DPos (Slvs_hEntity pointEntityId);

    Slvs_Constraint & constraint(Slvs_hConstraint constraintId);
    std::vector<Slvs_hConstraint> failedConstraints() const;

private:
    Slvs_System m_slvsSystem;

    std::vector<Slvs_Param>      * m_paramsMemory;
    std::vector<Slvs_Entity>     * m_entityMemory;
    std::vector<Slvs_Constraint> * m_constraintMemory;
    std::vector<Slvs_hConstraint>* m_failedConstrMemory;
};
 

