
#define EXPORT_DLL
#include "SolveSpaceSystem.h"
#include <assert.h>



//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
SolveSpaceSystem::SolveSpaceSystem()
    : m_paramsMemory     (new std::vector<Slvs_Param>       ())
    , m_entityMemory      (new std::vector<Slvs_Entity>     ())
    , m_constraintMemory  (new std::vector<Slvs_Constraint> ())
    , m_failedConstrMemory(new std::vector<Slvs_hConstraint>())

{
    m_paramsMemory    ->reserve(100);
    m_entityMemory    ->reserve(100);
    m_constraintMemory->reserve(100);
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
Slvs_hParam SolveSpaceSystem::addParam(Slvs_Param parameter)
{

    parameter.h = static_cast<Slvs_hParam>(m_paramsMemory->size()+1);
    m_paramsMemory->push_back(parameter);

    m_slvsSystem.param  =  m_paramsMemory->data();
    m_slvsSystem.params =  static_cast<int>(m_paramsMemory->size());

    return parameter.h;
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
Slvs_hEntity SolveSpaceSystem::addEntity(Slvs_Entity entity)
{
    entity.h = static_cast<Slvs_hEntity>(m_entityMemory->size()+1);
    m_entityMemory->push_back(entity);

    m_slvsSystem.entity   =  m_entityMemory->data();
    m_slvsSystem.entities = static_cast<int>(m_entityMemory->size());

    return entity.h;
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
Slvs_hConstraint SolveSpaceSystem::addConstr(Slvs_Constraint constr)
{
    constr.h = static_cast<Slvs_hConstraint>(m_constraintMemory->size()+1);
    m_constraintMemory->push_back(constr);

    m_slvsSystem.constraint  =  m_constraintMemory->data();
    m_slvsSystem.constraints = static_cast<int>(m_constraintMemory->size());

    return constr.h;
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
SolveSpaceSystem::ResultStatus SolveSpaceSystem::solve(Slvs_hGroup groupId, bool reportFailedConstraints /*= true*/)
{
    m_failedConstrMemory->resize(m_constraintMemory->size());

    m_slvsSystem.failed  =  m_failedConstrMemory->data();
    m_slvsSystem.faileds = static_cast<int>(m_failedConstrMemory->size());

    m_slvsSystem.calculateFaileds = reportFailedConstraints;

    Slvs_Solve(&m_slvsSystem, groupId);

    m_failedConstrMemory->resize(m_slvsSystem.faileds);

    return static_cast<ResultStatus>(m_slvsSystem.result);
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
double SolveSpaceSystem::parameterValue(Slvs_hParam paramId)
{
    return (*m_paramsMemory)[paramId-1].val;
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
void SolveSpaceSystem::setParameterValue(Slvs_hParam paramId, double value)
{
    (*m_paramsMemory)[paramId-1].val = value;
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
std::tuple< std::valarray<double>,
    std::valarray<double>,
    std::valarray<double> > SolveSpaceSystem::orientationMx(Slvs_hEntity normalIn3dEntityId)
{
    Slvs_Entity e_CS = (*m_entityMemory)[normalIn3dEntityId -1];
    if ( e_CS.type == SLVS_E_NORMAL_IN_3D )
    {
        std::valarray<double> quat ={ 0.0, 0.0, 0.0, 0.0 };
        quat[0] =  parameterValue(e_CS.param[0]);
        quat[1] =  parameterValue(e_CS.param[1]);
        quat[2] =  parameterValue(e_CS.param[2]);
        quat[3] =  parameterValue(e_CS.param[3]);
        std::valarray<double> Ex ={ 0.0,0.0,0.0 };
        std::valarray<double> Ey ={ 0.0,0.0,0.0 };
        std::valarray<double> Ez ={ 0.0,0.0,0.0 };

        Slvs_QuaternionU(quat[0], quat[1], quat[2], quat[3],
                         &Ex[0], &Ex[1], &Ex[2]);
        Slvs_QuaternionV(quat[0], quat[1], quat[2], quat[3],
                         &Ey[0], &Ey[1], &Ey[2]);
        Slvs_QuaternionN(quat[0], quat[1], quat[2], quat[3],
                         &Ez[0], &Ez[1], &Ez[2]);

        return std::make_tuple(Ex, Ey, Ez);
    }
    assert(false);
    return std::make_tuple(std::valarray<double>(), std::valarray<double>(), std::valarray<double>());
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
std::valarray<double> SolveSpaceSystem::global3DPos(Slvs_hEntity pointEntityId)
{
    std::valarray<double> point ={ 0.0,0.0,0.0 };

    Slvs_Entity pointEntity = (*m_entityMemory)[pointEntityId -1];
    if ( pointEntity.type == SLVS_E_POINT_IN_2D )
    {
        std::valarray<double> locPoint ={ 0.0,0.0,0.0 };
        locPoint[0] = parameterValue(pointEntity.param[0]);
        locPoint[1] = parameterValue(pointEntity.param[1]);

        Slvs_Entity e_Plane = (*m_entityMemory)[pointEntity.wrkpl - 1];
        std::valarray<double> origin = global3DPos(e_Plane.point[0]);
        auto mx = orientationMx(e_Plane.normal);
        point = origin + std::get<0>(mx)*locPoint[0] + std::get<1>(mx)*locPoint[1];

    }
    else if ( pointEntity.type == SLVS_E_POINT_IN_3D )
    {
        point[0] = parameterValue(pointEntity.param[0]);
        point[1] = parameterValue(pointEntity.param[1]);
        point[2] = parameterValue(pointEntity.param[2]);
    }

    return point;
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
Slvs_Constraint& SolveSpaceSystem::constraint(Slvs_hConstraint constraintId)
{
    return (*m_constraintMemory)[constraintId-1];
}

//--------------------------------------------------------------------------------------------------
/// 
//--------------------------------------------------------------------------------------------------
 std::vector<Slvs_hConstraint> SolveSpaceSystem::failedConstraints() const
{
    return (*m_failedConstrMemory);
}
