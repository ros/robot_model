// -*- coding: utf-8 -*-
/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2010, Willow Garage, Inc., University of Tokyo
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redstributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Authors: Rosen Diankov, Tim Field */

#include "collada_urdf/collada_urdf.h"
#include <map>
#include <vector>
#include <list>

#include <dae.h>
#include <dae/daeDocument.h>
#include <dae/daeErrorHandler.h>
#include <dae/domAny.h>
#include <dom/domCOLLADA.h>
#include <dom/domConstants.h>
#include <dom/domElements.h>
#include <dom/domTriangles.h>
#include <dom/domTypes.h>
#include <resource_retriever/retriever.h>
#include <urdf/model.h>
#include <urdf_interface/pose.h>
#include <angles/angles.h>
#include <ros/assert.h>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>
#include <boost/format.hpp>
#include <boost/array.hpp>

#include <assimp/assimp.hpp>
#include <assimp/aiScene.h>
#include <assimp/aiPostProcess.h>
#include <assimp/IOStream.h>
#include <assimp/IOSystem.h>

#define FOREACH(it, v) for(typeof((v).begin())it = (v).begin(); it != (v).end(); (it)++)
#define FOREACHC FOREACH

using namespace std;

namespace collada_urdf {

/// ResourceIOStream is copied from rviz (BSD, Willow Garage)
class ResourceIOStream : public Assimp::IOStream
{
public:
    ResourceIOStream(const resource_retriever::MemoryResource& res)
        : res_(res)
        , pos_(res.data.get())
    {
    }

    ~ResourceIOStream()
    {
    }

    size_t Read(void* buffer, size_t size, size_t count)
    {
        size_t to_read = size * count;
        if (pos_ + to_read > res_.data.get() + res_.size)
        {
            to_read = res_.size - (pos_ - res_.data.get());
        }

        memcpy(buffer, pos_, to_read);
        pos_ += to_read;

        return to_read;
    }

    size_t Write( const void* buffer, size_t size, size_t count) {
        ROS_BREAK(); return 0;
    }

    aiReturn Seek( size_t offset, aiOrigin origin)
    {
        uint8_t* new_pos = 0;
        switch (origin)
        {
        case aiOrigin_SET:
            new_pos = res_.data.get() + offset;
            break;
        case aiOrigin_CUR:
            new_pos = pos_ + offset; // TODO is this right?  can offset really not be negative
            break;
        case aiOrigin_END:
            new_pos = res_.data.get() + res_.size - offset; // TODO is this right?
            break;
        default:
            ROS_BREAK();
        }

        if (new_pos < res_.data.get() || new_pos > res_.data.get() + res_.size)
        {
            return aiReturn_FAILURE;
        }

        pos_ = new_pos;
        return aiReturn_SUCCESS;
    }

    size_t Tell() const
    {
        return pos_ - res_.data.get();
    }

    size_t FileSize() const
    {
        return res_.size;
    }

    void Flush() {
    }

private:
    resource_retriever::MemoryResource res_;
    uint8_t* pos_;
};

namespace mathextra {

// code from MagicSoftware by Dave Eberly
const double g_fEpsilon = 1e-15;
//===========================================================================


#define distinctRoots 0                       // roots r0 < r1 < r2
#define singleRoot 1                       // root r0
#define floatRoot01 2                       // roots r0 = r1 < r2
#define floatRoot12 4                       // roots r0 < r1 = r2
#define tripleRoot 6                       // roots r0 = r1 = r2


template <typename T, typename S>
void Tridiagonal3 (S* mat, T* diag, T* subd)
{
    T a, b, c, d, e, f, ell, q;

    a = mat[0*3+0];
    b = mat[0*3+1];
    c = mat[0*3+2];
    d = mat[1*3+1];
    e = mat[1*3+2];
    f = mat[2*3+2];

    subd[2] = 0.0;
    diag[0] = a;
    if ( fabs(c) >= g_fEpsilon ) {
        ell = (T)sqrt(b*b+c*c);
        b /= ell;
        c /= ell;
        q = 2*b*e+c*(f-d);
        diag[1] = d+c*q;
        diag[2] = f-c*q;
        subd[0] = ell;
        subd[1] = e-b*q;
        mat[0*3+0] = (S)1; mat[0*3+1] = (S)0; mat[0*3+2] = (T)0;
        mat[1*3+0] = (S)0; mat[1*3+1] = b; mat[1*3+2] = c;
        mat[2*3+0] = (S)0; mat[2*3+1] = c; mat[2*3+2] = -b;
    }
    else {
        diag[1] = d;
        diag[2] = f;
        subd[0] = b;
        subd[1] = e;
        mat[0*3+0] = (S)1; mat[0*3+1] = (S)0; mat[0*3+2] = (S)0;
        mat[1*3+0] = (S)0; mat[1*3+1] = (S)1; mat[1*3+2] = (S)0;
        mat[2*3+0] = (S)0; mat[2*3+1] = (S)0; mat[2*3+2] = (S)1;
    }
}

int CubicRoots (double c0, double c1, double c2, double *r0, double *r1, double *r2)
{
    // polynomial is L^3-c2*L^2+c1*L-c0

    int maxiter = 50;
    double discr, temp, pval, pdval, b0, b1;
    int i;

    // find local extrema (if any) of p'(L) = 3*L^2-2*c2*L+c1
    discr = c2*c2-3*c1;
    if ( discr >= 0.0 ) {
        discr = (double)sqrt(discr);
        temp = (c2+discr)/3;
        pval = temp*(temp*(temp-c2)+c1)-c0;
        if ( pval >= 0.0 ) {
            // double root occurs before the positive local maximum
            (*r0) = (c2-discr)/3 - 1;  // initial guess for Newton's methods
            pval = 2*g_fEpsilon;
            for (i = 0; i < maxiter && fabs(pval) > g_fEpsilon; i++) {
                pval = (*r0)*((*r0)*((*r0)-c2)+c1)-c0;
                pdval = (*r0)*(3*(*r0)-2*c2)+c1;
                (*r0) -= pval/pdval;
            }

            // Other two roots are solutions to quadratic equation
            // L^2 + ((*r0)-c2)*L + [(*r0)*((*r0)-c2)+c1] = 0.
            b1 = (*r0)-c2;
            b0 = (*r0)*((*r0)-c2)+c1;
            discr = b1*b1-4*b0;
            if ( discr < -g_fEpsilon )
            {
                // single root r0
                return singleRoot;
            }
            else
            {
                int result = distinctRoots;

                // roots r0 <= r1 <= r2
                discr = sqrt(fabs(discr));
                (*r1) = 0.5f*(-b1-discr);
                (*r2) = 0.5f*(-b1+discr);

                if ( fabs((*r0)-(*r1)) <= g_fEpsilon )
                {
                    (*r0) = (*r1);
                    result |= floatRoot01;
                }
                if ( fabs((*r1)-(*r2)) <= g_fEpsilon )
                {
                    (*r1) = (*r2);
                    result |= floatRoot12;
                }
                return result;
            }
        }
        else {
            // double root occurs after the negative local minimum
            (*r2) = temp + 1;  // initial guess for Newton's method
            pval = 2*g_fEpsilon;
            for (i = 0; i < maxiter && fabs(pval) > g_fEpsilon; i++) {
                pval = (*r2)*((*r2)*((*r2)-c2)+c1)-c0;
                pdval = (*r2)*(3*(*r2)-2*c2)+c1;
                (*r2) -= pval/pdval;
            }

            // Other two roots are solutions to quadratic equation
            // L^2 + (r2-c2)*L + [r2*(r2-c2)+c1] = 0.
            b1 = (*r2)-c2;
            b0 = (*r2)*((*r2)-c2)+c1;
            discr = b1*b1-4*b0;
            if ( discr < -g_fEpsilon )
            {
                // single root
                (*r0) = (*r2);
                return singleRoot;
            }
            else
            {
                int result = distinctRoots;

                // roots r0 <= r1 <= r2
                discr = sqrt(fabs(discr));
                (*r0) = 0.5f*(-b1-discr);
                (*r1) = 0.5f*(-b1+discr);

                if ( fabs((*r0)-(*r1)) <= g_fEpsilon )
                {
                    (*r0) = (*r1);
                    result |= floatRoot01;
                }
                if ( fabs((*r1)-(*r2)) <= g_fEpsilon )
                {
                    (*r1) = (*r2);
                    result |= floatRoot12;
                }
                return result;
            }
        }
    }
    else {
        // p(L) has one double root
        (*r0) = c0;
        pval = 2*g_fEpsilon;
        for (i = 0; i < maxiter && fabs(pval) > g_fEpsilon; i++) {
            pval = (*r0)*((*r0)*((*r0)-c2)+c1)-c0;
            pdval = (*r0)*(3*(*r0)-2*c2)+c1;
            (*r0) -= pval/pdval;
        }
        return singleRoot;
    }
}

//----------------------------------------------------------------------------
template <class T>
bool _QLAlgorithm3 (T* m_aafEntry, T* afDiag, T* afSubDiag)
{
    // QL iteration with implicit shifting to reduce matrix from tridiagonal
    // to diagonal

    for (int i0 = 0; i0 < 3; i0++)
    {
        const int iMaxIter = 32;
        int iIter;
        for (iIter = 0; iIter < iMaxIter; iIter++)
        {
            int i1;
            for (i1 = i0; i1 <= 1; i1++)
            {
                T fSum = fabs(afDiag[i1]) +
                         fabs(afDiag[i1+1]);
                if ( fabs(afSubDiag[i1]) + fSum == fSum )
                    break;
            }
            if ( i1 == i0 )
                break;

            T fTmp0 = (afDiag[i0+1]-afDiag[i0])/(2.0f*afSubDiag[i0]);
            T fTmp1 = sqrt(fTmp0*fTmp0+1.0f);
            if ( fTmp0 < 0.0f )
                fTmp0 = afDiag[i1]-afDiag[i0]+afSubDiag[i0]/(fTmp0-fTmp1);
            else
                fTmp0 = afDiag[i1]-afDiag[i0]+afSubDiag[i0]/(fTmp0+fTmp1);
            T fSin = 1.0f;
            T fCos = 1.0f;
            T fTmp2 = 0.0f;
            for (int i2 = i1-1; i2 >= i0; i2--)
            {
                T fTmp3 = fSin*afSubDiag[i2];
                T fTmp4 = fCos*afSubDiag[i2];
                if ( fabs(fTmp3) >= fabs(fTmp0) )
                {
                    fCos = fTmp0/fTmp3;
                    fTmp1 = sqrt(fCos*fCos+1.0f);
                    afSubDiag[i2+1] = fTmp3*fTmp1;
                    fSin = 1.0f/fTmp1;
                    fCos *= fSin;
                }
                else
                {
                    fSin = fTmp3/fTmp0;
                    fTmp1 = sqrt(fSin*fSin+1.0f);
                    afSubDiag[i2+1] = fTmp0*fTmp1;
                    fCos = 1.0f/fTmp1;
                    fSin *= fCos;
                }
                fTmp0 = afDiag[i2+1]-fTmp2;
                fTmp1 = (afDiag[i2]-fTmp0)*fSin+2.0f*fTmp4*fCos;
                fTmp2 = fSin*fTmp1;
                afDiag[i2+1] = fTmp0+fTmp2;
                fTmp0 = fCos*fTmp1-fTmp4;

                for (int iRow = 0; iRow < 3; iRow++)
                {
                    fTmp3 = m_aafEntry[iRow*3+i2+1];
                    m_aafEntry[iRow*3+i2+1] = fSin*m_aafEntry[iRow*3+i2] +
                                              fCos*fTmp3;
                    m_aafEntry[iRow*3+i2] = fCos*m_aafEntry[iRow*3+i2] -
                                            fSin*fTmp3;
                }
            }
            afDiag[i0] -= fTmp2;
            afSubDiag[i0] = fTmp0;
            afSubDiag[i1] = 0.0f;
        }

        if ( iIter == iMaxIter )
        {
            // should not get here under normal circumstances
            return false;
        }
    }

    return true;
}

bool QLAlgorithm3 (float* m_aafEntry, float* afDiag, float* afSubDiag)
{
    return _QLAlgorithm3<float>(m_aafEntry, afDiag, afSubDiag);
}

bool QLAlgorithm3 (double* m_aafEntry, double* afDiag, double* afSubDiag)
{
    return _QLAlgorithm3<double>(m_aafEntry, afDiag, afSubDiag);
}

void EigenSymmetric3(const double* fmat, double* afEigenvalue, double* fevecs)
{
    double afSubDiag[3];

    memcpy(fevecs, fmat, sizeof(double)*9);
    Tridiagonal3(fevecs, afEigenvalue,afSubDiag);
    QLAlgorithm3(fevecs, afEigenvalue,afSubDiag);

    // make eigenvectors form a right--handed system
    double fDet = fevecs[0*3+0] * (fevecs[1*3+1] * fevecs[2*3+2] - fevecs[1*3+2] * fevecs[2*3+1]) +
                  fevecs[0*3+1] * (fevecs[1*3+2] * fevecs[2*3+0] - fevecs[1*3+0] * fevecs[2*3+2]) +
                  fevecs[0*3+2] * (fevecs[1*3+0] * fevecs[2*3+1] - fevecs[1*3+1] * fevecs[2*3+0]);
    if ( fDet < 0.0f )
    {
        fevecs[0*3+2] = -fevecs[0*3+2];
        fevecs[1*3+2] = -fevecs[1*3+2];
        fevecs[2*3+2] = -fevecs[2*3+2];
    }
}
/* end of MAGIC code */

} // end namespace geometry

/// ResourceIOSystem is copied from rviz (BSD, Willow Garage)
class ResourceIOSystem : public Assimp::IOSystem
{
public:
    ResourceIOSystem()
    {
    }

    ~ResourceIOSystem()
    {
    }

    // Check whether a specific file exists
    bool Exists(const char* file) const
    {
        // Ugly -- two retrievals where there should be one (Exists + Open)
        // resource_retriever needs a way of checking for existence
        // TODO: cache this
        resource_retriever::MemoryResource res;
        try {
            res = retriever_.get(file);
        }
        catch (resource_retriever::Exception& e) {
            return false;
        }

        return true;
    }

    // Get the path delimiter character we'd like to see
    char getOsSeparator() const
    {
        return '/';
    }

    // ... and finally a method to open a custom stream
    Assimp::IOStream* Open(const char* file, const char* mode)
    {
        ROS_ASSERT(mode == std::string("r") || mode == std::string("rb"));

        // Ugly -- two retrievals where there should be one (Exists + Open)
        // resource_retriever needs a way of checking for existence
        resource_retriever::MemoryResource res;
        try {
            res = retriever_.get(file);
        }
        catch (resource_retriever::Exception& e) {
            return 0;
        }

        return new ResourceIOStream(res);
    }

    void Close(Assimp::IOStream* stream) {
        delete stream;
    }

private:
    mutable resource_retriever::Retriever retriever_;
};


class Point
{
public:
    Point(urdf::Vector3 _p1, urdf::Vector3 _p2, urdf::Vector3 _p3) { this->p1 = _p1; this->p2 = _p2; this->p3 = _p3;};
    Point() { this->clear(); };
    urdf::Vector3 p1, p2, p3;

    void clear() { p1.clear(); p2.clear(); p3.clear(); };
};

/// \brief Implements writing urdf::Model objects to a COLLADA DOM.
class ColladaWriter : public daeErrorHandler
{
private:
    struct SCENE
    {
        domVisual_sceneRef vscene;
        domKinematics_sceneRef kscene;
        domPhysics_sceneRef pscene;
        domInstance_with_extraRef viscene;
        domInstance_kinematics_sceneRef kiscene;
        domInstance_with_extraRef piscene;
    };

    typedef std::map< boost::shared_ptr<const urdf::Link>, urdf::Pose > MAPLINKPOSES;
    struct LINKOUTPUT
    {
        list<pair<int,string> > listusedlinks;
        list<pair<int,string> > listprocesseddofs;
        daeElementRef plink;
        domNodeRef pnode;
        MAPLINKPOSES _maplinkposes;
    };

    struct physics_model_output
    {
        domPhysics_modelRef pmodel;
        std::vector<std::string > vrigidbodysids;     ///< same ordering as the physics indices
    };

    struct kinematics_model_output
    {
        struct axis_output
        {
            //axis_output(const string& sid, KinBody::JointConstPtr pjoint, int iaxis) : sid(sid), pjoint(pjoint), iaxis(iaxis) {}
            axis_output() : iaxis(0) {
            }
            string sid, nodesid;
            boost::shared_ptr<const urdf::Joint> pjoint;
            int iaxis;
            string jointnodesid;
        };
        domKinematics_modelRef kmodel;
        std::vector<axis_output> vaxissids;
        std::vector<std::string > vlinksids;
        MAPLINKPOSES _maplinkposes;
    };

    struct axis_sids
    {
        axis_sids(const string& axissid, const string& valuesid, const string& jointnodesid) : axissid(axissid), valuesid(valuesid), jointnodesid(jointnodesid) {
        }
        string axissid, valuesid, jointnodesid;
    };

    struct instance_kinematics_model_output
    {
        domInstance_kinematics_modelRef ikm;
        std::vector<axis_sids> vaxissids;
        boost::shared_ptr<kinematics_model_output> kmout;
        std::vector<std::pair<std::string,std::string> > vkinematicsbindings;
    };

    struct instance_articulated_system_output
    {
        domInstance_articulated_systemRef ias;
        std::vector<axis_sids> vaxissids;
        std::vector<std::string > vlinksids;
        std::vector<std::pair<std::string,std::string> > vkinematicsbindings;
    };

    struct instance_physics_model_output
    {
        domInstance_physics_modelRef ipm;
        boost::shared_ptr<physics_model_output> pmout;
    };

    struct kinbody_models
    {
        std::string uri, kinematicsgeometryhash;
        boost::shared_ptr<kinematics_model_output> kmout;
        boost::shared_ptr<physics_model_output> pmout;
    };

public:
    ColladaWriter(const urdf::Model& robot, int writeoptions) : _writeoptions(writeoptions), _robot(robot), _dom(NULL) {
        daeErrorHandler::setErrorHandler(this);
        _collada.reset(new DAE);
        _collada->setIOPlugin(NULL);
        _collada->setDatabase(NULL);
        _importer.SetIOHandler(new ResourceIOSystem());
    }
    virtual ~ColladaWriter() {
    }

    boost::shared_ptr<DAE> convert()
    {
        try {
            const char* documentName = "urdf_snapshot";
            daeDocument *doc = NULL;
            daeInt error = _collada->getDatabase()->insertDocument(documentName, &doc ); // also creates a collada root
            if (error != DAE_OK || doc == NULL) {
                throw ColladaUrdfException("Failed to create document");
            }
            _dom = daeSafeCast<domCOLLADA>(doc->getDomRoot());
            _dom->setAttribute("xmlns:math","http://www.w3.org/1998/Math/MathML");

            //create the required asset tag
            domAssetRef asset = daeSafeCast<domAsset>( _dom->add( COLLADA_ELEMENT_ASSET ) );
            {
                // facet becomes owned by locale, so no need to explicitly delete
                boost::posix_time::time_facet* facet = new boost::posix_time::time_facet("%Y-%m-%dT%H:%M:%s");
                std::stringstream ss;
                ss.imbue(std::locale(ss.getloc(), facet));
                ss << boost::posix_time::second_clock::local_time();

                domAsset::domCreatedRef created = daeSafeCast<domAsset::domCreated>( asset->add( COLLADA_ELEMENT_CREATED ) );
                created->setValue(ss.str().c_str());
                domAsset::domModifiedRef modified = daeSafeCast<domAsset::domModified>( asset->add( COLLADA_ELEMENT_MODIFIED ) );
                modified->setValue(ss.str().c_str());

                domAsset::domContributorRef contrib = daeSafeCast<domAsset::domContributor>( asset->add( COLLADA_TYPE_CONTRIBUTOR ) );
                domAsset::domContributor::domAuthoring_toolRef authoringtool = daeSafeCast<domAsset::domContributor::domAuthoring_tool>( contrib->add( COLLADA_ELEMENT_AUTHORING_TOOL ) );
                authoringtool->setValue("URDF Collada Writer");

                domAsset::domUnitRef units = daeSafeCast<domAsset::domUnit>( asset->add( COLLADA_ELEMENT_UNIT ) );
                units->setMeter(1);
                units->setName("meter");

                domAsset::domUp_axisRef zup = daeSafeCast<domAsset::domUp_axis>( asset->add( COLLADA_ELEMENT_UP_AXIS ) );
                zup->setValue(UP_AXIS_Z_UP);
            }

            _globalscene = _dom->getScene();
            if( !_globalscene ) {
                _globalscene = daeSafeCast<domCOLLADA::domScene>( _dom->add( COLLADA_ELEMENT_SCENE ) );
            }

            _visualScenesLib = daeSafeCast<domLibrary_visual_scenes>(_dom->add(COLLADA_ELEMENT_LIBRARY_VISUAL_SCENES));
            _visualScenesLib->setId("vscenes");
            _geometriesLib = daeSafeCast<domLibrary_geometries>(_dom->add(COLLADA_ELEMENT_LIBRARY_GEOMETRIES));
            _geometriesLib->setId("geometries");
            _effectsLib = daeSafeCast<domLibrary_effects>(_dom->add(COLLADA_ELEMENT_LIBRARY_EFFECTS));
            _effectsLib->setId("effects");
            _materialsLib = daeSafeCast<domLibrary_materials>(_dom->add(COLLADA_ELEMENT_LIBRARY_MATERIALS));
            _materialsLib->setId("materials");
            _kinematicsModelsLib = daeSafeCast<domLibrary_kinematics_models>(_dom->add(COLLADA_ELEMENT_LIBRARY_KINEMATICS_MODELS));
            _kinematicsModelsLib->setId("kmodels");
            _articulatedSystemsLib = daeSafeCast<domLibrary_articulated_systems>(_dom->add(COLLADA_ELEMENT_LIBRARY_ARTICULATED_SYSTEMS));
            _articulatedSystemsLib->setId("asystems");
            _kinematicsScenesLib = daeSafeCast<domLibrary_kinematics_scenes>(_dom->add(COLLADA_ELEMENT_LIBRARY_KINEMATICS_SCENES));
            _kinematicsScenesLib->setId("kscenes");
            _physicsScenesLib = daeSafeCast<domLibrary_physics_scenes>(_dom->add(COLLADA_ELEMENT_LIBRARY_PHYSICS_SCENES));
            _physicsScenesLib->setId("pscenes");
            _physicsModelsLib = daeSafeCast<domLibrary_physics_models>(_dom->add(COLLADA_ELEMENT_LIBRARY_PHYSICS_MODELS));
            _physicsModelsLib->setId("pmodels");
            domExtraRef pextra_library_sensors = daeSafeCast<domExtra>(_dom->add(COLLADA_ELEMENT_EXTRA));
            pextra_library_sensors->setId("sensors");
            pextra_library_sensors->setType("library_sensors");
            _sensorsLib = daeSafeCast<domTechnique>(pextra_library_sensors->add(COLLADA_ELEMENT_TECHNIQUE));
            _sensorsLib->setProfile("OpenRAVE"); ///< documented profile on robot extensions

            _CreateScene();
            _WritePhysics();
            _WriteRobot();
            _WriteBindingsInstance_kinematics_scene();
            return _collada;
        }
        catch (ColladaUrdfException ex) {
            ROS_ERROR("Error converting: %s", ex.what());
            return boost::shared_ptr<DAE>();
        }
    }

protected:
    virtual void handleError(daeString msg) {
        throw ColladaUrdfException(msg);
    }
    virtual void handleWarning(daeString msg) {
        std::cerr << "COLLADA DOM warning: " << msg << std::endl;
    }

    void _CreateScene()
    {
        // Create visual scene
        _scene.vscene = daeSafeCast<domVisual_scene>(_visualScenesLib->add(COLLADA_ELEMENT_VISUAL_SCENE));
        _scene.vscene->setId("vscene");
        _scene.vscene->setName("URDF Visual Scene");

        // Create kinematics scene
        _scene.kscene = daeSafeCast<domKinematics_scene>(_kinematicsScenesLib->add(COLLADA_ELEMENT_KINEMATICS_SCENE));
        _scene.kscene->setId("kscene");
        _scene.kscene->setName("URDF Kinematics Scene");

        // Create physic scene
        _scene.pscene = daeSafeCast<domPhysics_scene>(_physicsScenesLib->add(COLLADA_ELEMENT_PHYSICS_SCENE));
        _scene.pscene->setId("pscene");
        _scene.pscene->setName("URDF Physics Scene");

        // Create instance visual scene
        _scene.viscene = daeSafeCast<domInstance_with_extra>(_globalscene->add( COLLADA_ELEMENT_INSTANCE_VISUAL_SCENE ));
        _scene.viscene->setUrl( (string("#") + string(_scene.vscene->getID())).c_str() );

        // Create instance kinematics scene
        _scene.kiscene = daeSafeCast<domInstance_kinematics_scene>(_globalscene->add( COLLADA_ELEMENT_INSTANCE_KINEMATICS_SCENE ));
        _scene.kiscene->setUrl( (string("#") + string(_scene.kscene->getID())).c_str() );

        // Create instance physics scene
        _scene.piscene = daeSafeCast<domInstance_with_extra>(_globalscene->add( COLLADA_ELEMENT_INSTANCE_PHYSICS_SCENE ));
        _scene.piscene->setUrl( (string("#") + string(_scene.pscene->getID())).c_str() );
    }

    void _WritePhysics() {
        domPhysics_scene::domTechnique_commonRef common = daeSafeCast<domPhysics_scene::domTechnique_common>(_scene.pscene->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
        //  Create gravity
        domTargetable_float3Ref g = daeSafeCast<domTargetable_float3>(common->add(COLLADA_ELEMENT_GRAVITY));
        g->getValue().set3 (0,0,0);
    }

    /// \brief Write kinematic body in a given scene
    void _WriteRobot(int id = 0)
    {
        ROS_DEBUG_STREAM(str(boost::format("writing robot as instance_articulated_system (%d) %s\n")%id%_robot.getName()));
        string asid = _ComputeId(str(boost::format("robot%d")%id));
        string askid = _ComputeId(str(boost::format("%s_kinematics")%asid));
        string asmid = _ComputeId(str(boost::format("%s_motion")%asid));
        string iassid = _ComputeId(str(boost::format("%s_inst")%asmid));

        domInstance_articulated_systemRef ias = daeSafeCast<domInstance_articulated_system>(_scene.kscene->add(COLLADA_ELEMENT_INSTANCE_ARTICULATED_SYSTEM));
        ias->setSid(iassid.c_str());
        ias->setUrl((string("#")+asmid).c_str());
        ias->setName(_robot.getName().c_str());

        _iasout.reset(new instance_articulated_system_output());
        _iasout->ias = ias;

        // motion info
        domArticulated_systemRef articulated_system_motion = daeSafeCast<domArticulated_system>(_articulatedSystemsLib->add(COLLADA_ELEMENT_ARTICULATED_SYSTEM));
        articulated_system_motion->setId(asmid.c_str());
        domMotionRef motion = daeSafeCast<domMotion>(articulated_system_motion->add(COLLADA_ELEMENT_MOTION));
        domMotion_techniqueRef mt = daeSafeCast<domMotion_technique>(motion->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
        domInstance_articulated_systemRef ias_motion = daeSafeCast<domInstance_articulated_system>(motion->add(COLLADA_ELEMENT_INSTANCE_ARTICULATED_SYSTEM));
        ias_motion->setUrl(str(boost::format("#%s")%askid).c_str());

        // kinematics info
        domArticulated_systemRef articulated_system_kinematics = daeSafeCast<domArticulated_system>(_articulatedSystemsLib->add(COLLADA_ELEMENT_ARTICULATED_SYSTEM));
        articulated_system_kinematics->setId(askid.c_str());
        domKinematicsRef kinematics = daeSafeCast<domKinematics>(articulated_system_kinematics->add(COLLADA_ELEMENT_KINEMATICS));
        domKinematics_techniqueRef kt = daeSafeCast<domKinematics_technique>(kinematics->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));

        _WriteInstance_kinematics_model(kinematics,askid,id);

        for(size_t idof = 0; idof < _ikmout->vaxissids.size(); ++idof) {
            string axis_infosid = _ComputeId(str(boost::format("axis_info_inst%d")%idof));
            boost::shared_ptr<const urdf::Joint> pjoint = _ikmout->kmout->vaxissids.at(idof).pjoint;
            BOOST_ASSERT(_mapjointindices[pjoint] == (int)idof);
            //int iaxis = _ikmout->kmout->vaxissids.at(idof).iaxis;

            //  Kinematics axis info
            domKinematics_axis_infoRef kai = daeSafeCast<domKinematics_axis_info>(kt->add(COLLADA_ELEMENT_AXIS_INFO));
            kai->setAxis(str(boost::format("%s/%s")%_ikmout->kmout->kmodel->getID()%_ikmout->kmout->vaxissids.at(idof).sid).c_str());
            kai->setSid(axis_infosid.c_str());
            bool bactive = !pjoint->mimic;
            double flower=0, fupper=0;
            if( pjoint->type != urdf::Joint::CONTINUOUS ) {
                if( !!pjoint->limits ) {
                    flower = pjoint->limits->lower;
                    fupper = pjoint->limits->upper;
                }
                if( !!pjoint->safety ) {
                    flower = pjoint->safety->soft_lower_limit;
                    fupper = pjoint->safety->soft_upper_limit;
                }
                if( flower == fupper ) {
                    bactive = false;
                }
                double fmult = 1.0;
                if( pjoint->type != urdf::Joint::PRISMATIC ) {
                    fmult = 180.0/M_PI;
                }
                domKinematics_limitsRef plimits = daeSafeCast<domKinematics_limits>(kai->add(COLLADA_ELEMENT_LIMITS));
                daeSafeCast<domCommon_float_or_param::domFloat>(plimits->add(COLLADA_ELEMENT_MIN)->add(COLLADA_ELEMENT_FLOAT))->setValue(flower*fmult);
                daeSafeCast<domCommon_float_or_param::domFloat>(plimits->add(COLLADA_ELEMENT_MAX)->add(COLLADA_ELEMENT_FLOAT))->setValue(fupper*fmult);
            }

            domCommon_bool_or_paramRef active = daeSafeCast<domCommon_bool_or_param>(kai->add(COLLADA_ELEMENT_ACTIVE));
            daeSafeCast<domCommon_bool_or_param::domBool>(active->add(COLLADA_ELEMENT_BOOL))->setValue(bactive);
            domCommon_bool_or_paramRef locked = daeSafeCast<domCommon_bool_or_param>(kai->add(COLLADA_ELEMENT_LOCKED));
            daeSafeCast<domCommon_bool_or_param::domBool>(locked->add(COLLADA_ELEMENT_BOOL))->setValue(false);

            //  Motion axis info
            domMotion_axis_infoRef mai = daeSafeCast<domMotion_axis_info>(mt->add(COLLADA_ELEMENT_AXIS_INFO));
            mai->setAxis(str(boost::format("%s/%s")%askid%axis_infosid).c_str());
            if( !!pjoint->limits ) {
                domCommon_float_or_paramRef speed = daeSafeCast<domCommon_float_or_param>(mai->add(COLLADA_ELEMENT_SPEED));
                daeSafeCast<domCommon_float_or_param::domFloat>(speed->add(COLLADA_ELEMENT_FLOAT))->setValue(pjoint->limits->velocity);
                domCommon_float_or_paramRef accel = daeSafeCast<domCommon_float_or_param>(mai->add(COLLADA_ELEMENT_ACCELERATION));
                daeSafeCast<domCommon_float_or_param::domFloat>(accel->add(COLLADA_ELEMENT_FLOAT))->setValue(pjoint->limits->effort);
            }
        }

        // write the bindings
        string asmsym = _ComputeId(str(boost::format("%s_%s")%asmid%_ikmout->ikm->getSid()));
        string assym = _ComputeId(str(boost::format("%s_%s")%_scene.kscene->getID()%_ikmout->ikm->getSid()));
        FOREACH(it, _ikmout->vkinematicsbindings) {
            domKinematics_newparamRef abm = daeSafeCast<domKinematics_newparam>(ias_motion->add(COLLADA_ELEMENT_NEWPARAM));
            abm->setSid(asmsym.c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(abm->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s")%askid%it->first).c_str());
            domKinematics_bindRef ab = daeSafeCast<domKinematics_bind>(ias->add(COLLADA_ELEMENT_BIND));
            ab->setSymbol(assym.c_str());
            daeSafeCast<domKinematics_param>(ab->add(COLLADA_ELEMENT_PARAM))->setRef(str(boost::format("%s/%s")%asmid%asmsym).c_str());
            _iasout->vkinematicsbindings.push_back(make_pair(string(ab->getSymbol()), it->second));
        }
        for(size_t idof = 0; idof < _ikmout->vaxissids.size(); ++idof) {
            const axis_sids& kas = _ikmout->vaxissids.at(idof);
            domKinematics_newparamRef abm = daeSafeCast<domKinematics_newparam>(ias_motion->add(COLLADA_ELEMENT_NEWPARAM));
            abm->setSid(_ComputeId(str(boost::format("%s_%s")%asmid%kas.axissid)).c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(abm->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s")%askid%kas.axissid).c_str());
            domKinematics_bindRef ab = daeSafeCast<domKinematics_bind>(ias->add(COLLADA_ELEMENT_BIND));
            ab->setSymbol(str(boost::format("%s_%s")%assym%kas.axissid).c_str());
            daeSafeCast<domKinematics_param>(ab->add(COLLADA_ELEMENT_PARAM))->setRef(str(boost::format("%s/%s_%s")%asmid%asmid%kas.axissid).c_str());
            string valuesid;
            if( kas.valuesid.size() > 0 ) {
                domKinematics_newparamRef abmvalue = daeSafeCast<domKinematics_newparam>(ias_motion->add(COLLADA_ELEMENT_NEWPARAM));
                abmvalue->setSid(_ComputeId(str(boost::format("%s_%s")%asmid%kas.valuesid)).c_str());
                daeSafeCast<domKinematics_newparam::domSIDREF>(abmvalue->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s")%askid%kas.valuesid).c_str());
                domKinematics_bindRef abvalue = daeSafeCast<domKinematics_bind>(ias->add(COLLADA_ELEMENT_BIND));
                valuesid = _ComputeId(str(boost::format("%s_%s")%assym%kas.valuesid));
                abvalue->setSymbol(valuesid.c_str());
                daeSafeCast<domKinematics_param>(abvalue->add(COLLADA_ELEMENT_PARAM))->setRef(str(boost::format("%s/%s_%s")%asmid%asmid%kas.valuesid).c_str());
            }
            _iasout->vaxissids.push_back(axis_sids(ab->getSymbol(),valuesid,kas.jointnodesid));
        }

        boost::shared_ptr<instance_physics_model_output> ipmout = _WriteInstance_physics_model(id,_scene.pscene,_scene.pscene->getID(), _ikmout->kmout->_maplinkposes);
    }

    /// \brief Write kinematic body in a given scene
    virtual void _WriteInstance_kinematics_model(daeElementRef parent, const string& sidscope, int id)
    {
        ROS_DEBUG_STREAM(str(boost::format("writing instance_kinematics_model %s\n")%_robot.getName()));
        boost::shared_ptr<kinematics_model_output> kmout = WriteKinematics_model(id);

        _ikmout.reset(new instance_kinematics_model_output());
        _ikmout->kmout = kmout;
        _ikmout->ikm = daeSafeCast<domInstance_kinematics_model>(parent->add(COLLADA_ELEMENT_INSTANCE_KINEMATICS_MODEL));

        string symscope, refscope;
        if( sidscope.size() > 0 ) {
            symscope = sidscope+string("_");
            refscope = sidscope+string("/");
        }
        string ikmsid = _ComputeId(str(boost::format("%s_inst")%kmout->kmodel->getID()));
        _ikmout->ikm->setUrl(str(boost::format("#%s")%kmout->kmodel->getID()).c_str());
        _ikmout->ikm->setSid(ikmsid.c_str());

        domKinematics_newparamRef kbind = daeSafeCast<domKinematics_newparam>(_ikmout->ikm->add(COLLADA_ELEMENT_NEWPARAM));
        kbind->setSid(_ComputeId(symscope+ikmsid).c_str());
        daeSafeCast<domKinematics_newparam::domSIDREF>(kbind->add(COLLADA_ELEMENT_SIDREF))->setValue((refscope+ikmsid).c_str());
        _ikmout->vkinematicsbindings.push_back(make_pair(string(kbind->getSid()), str(boost::format("visual%d/node%d")%id%_maplinkindices[_robot.getRoot()])));

        _ikmout->vaxissids.reserve(kmout->vaxissids.size());
        int i = 0;
        FOREACH(it,kmout->vaxissids) {
            domKinematics_newparamRef kbind = daeSafeCast<domKinematics_newparam>(_ikmout->ikm->add(COLLADA_ELEMENT_NEWPARAM));
            string ref = it->sid;
            size_t index = ref.find("/");
            while(index != string::npos) {
                ref[index] = '.';
                index = ref.find("/",index+1);
            }
            string sid = _ComputeId(symscope+ikmsid+"_"+ref);
            kbind->setSid(sid.c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(kbind->add(COLLADA_ELEMENT_SIDREF))->setValue((refscope+ikmsid+"/"+it->sid).c_str());
            double value=0;
            double flower=0, fupper=0;
            if( !!it->pjoint->limits ) {
                flower = it->pjoint->limits->lower;
                fupper = it->pjoint->limits->upper;
            }
            if( flower > 0 || fupper < 0 ) {
                value = 0.5*(flower+fupper);
            }
            domKinematics_newparamRef pvalueparam = daeSafeCast<domKinematics_newparam>(_ikmout->ikm->add(COLLADA_ELEMENT_NEWPARAM));
            pvalueparam->setSid((sid+string("_value")).c_str());
            daeSafeCast<domKinematics_newparam::domFloat>(pvalueparam->add(COLLADA_ELEMENT_FLOAT))->setValue(value);
            _ikmout->vaxissids.push_back(axis_sids(sid,pvalueparam->getSid(),kmout->vaxissids.at(i).jointnodesid));
            ++i;
        }
    }

    virtual boost::shared_ptr<kinematics_model_output> WriteKinematics_model(int id)
    {
        domKinematics_modelRef kmodel = daeSafeCast<domKinematics_model>(_kinematicsModelsLib->add(COLLADA_ELEMENT_KINEMATICS_MODEL));
        string kmodelid = _ComputeKinematics_modelId(id);
        kmodel->setId(kmodelid.c_str());
        kmodel->setName(_robot.getName().c_str());

        domKinematics_model_techniqueRef ktec = daeSafeCast<domKinematics_model_technique>(kmodel->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));

        //  Create root node for the visual scene
        domNodeRef pnoderoot = daeSafeCast<domNode>(_scene.vscene->add(COLLADA_ELEMENT_NODE));
        string bodyid = _ComputeId(str(boost::format("visual%d")%id));
        pnoderoot->setId(bodyid.c_str());
        pnoderoot->setSid(bodyid.c_str());
        pnoderoot->setName(_robot.getName().c_str());

        //  Declare all the joints
        _mapjointindices.clear();
        int index=0;
        FOREACHC(itj, _robot.joints_) {
            _mapjointindices[itj->second] = index++;
        }
        _maplinkindices.clear();
        index=0;
        FOREACHC(itj, _robot.links_) {
            _maplinkindices[itj->second] = index++;
        }
        _mapmaterialindices.clear();
        index=0;
        FOREACHC(itj, _robot.materials_) {
            _mapmaterialindices[itj->second] = index++;
        }

        double lmin, lmax;
        vector<domJointRef> vdomjoints(_robot.joints_.size());
        boost::shared_ptr<kinematics_model_output> kmout(new kinematics_model_output());
        kmout->kmodel = kmodel;
        kmout->vaxissids.resize(_robot.joints_.size());
        kmout->vlinksids.resize(_robot.links_.size());

        FOREACHC(itjoint, _robot.joints_) {
            boost::shared_ptr<urdf::Joint> pjoint = itjoint->second;
            int index = _mapjointindices[itjoint->second];
            domJointRef pdomjoint = daeSafeCast<domJoint>(ktec->add(COLLADA_ELEMENT_JOINT));
            string jointid = _ComputeId(pjoint->name); //str(boost::format("joint%d")%index);
            pdomjoint->setSid(jointid.c_str() );
            pdomjoint->setName(pjoint->name.c_str());
            domAxis_constraintRef axis;
            if( !!pjoint->limits ) {
                lmin=pjoint->limits->lower;
                lmax=pjoint->limits->upper;
            }
            else {
                lmin = lmax = 0;
            }

            double fmult = 1.0;
            switch(pjoint->type) {
            case urdf::Joint::REVOLUTE:
            case urdf::Joint::CONTINUOUS:
                axis = daeSafeCast<domAxis_constraint>(pdomjoint->add(COLLADA_ELEMENT_REVOLUTE));
                fmult = 180.0f/M_PI;
                lmin*=fmult;
                lmax*=fmult;
                break;
            case urdf::Joint::PRISMATIC:
                axis = daeSafeCast<domAxis_constraint>(pdomjoint->add(COLLADA_ELEMENT_PRISMATIC));
                break;
            case urdf::Joint::FIXED:
                axis = daeSafeCast<domAxis_constraint>(pdomjoint->add(COLLADA_ELEMENT_REVOLUTE));
                lmin = 0;
                lmax = 0;
                fmult = 0;
                break;
            default:
                ROS_WARN_STREAM(str(boost::format("unsupported joint type specified %d")%(int)pjoint->type));
                break;
            }

            if( !axis ) {
                continue;
            }

            int ia = 0;
            string axisid = _ComputeId(str(boost::format("axis%d")%ia));
            axis->setSid(axisid.c_str());
            kmout->vaxissids.at(index).pjoint = pjoint;
            kmout->vaxissids.at(index).sid = jointid+string("/")+axisid;
            kmout->vaxissids.at(index).iaxis = ia;
            domAxisRef paxis = daeSafeCast<domAxis>(axis->add(COLLADA_ELEMENT_AXIS));
            paxis->getValue().setCount(3);
            paxis->getValue()[0] = pjoint->axis.x;
            paxis->getValue()[1] = pjoint->axis.y;
            paxis->getValue()[2] = pjoint->axis.z;
            if( pjoint->type != urdf::Joint::CONTINUOUS ) {
                domJoint_limitsRef plimits = daeSafeCast<domJoint_limits>(axis->add(COLLADA_TYPE_LIMITS));
                daeSafeCast<domMinmax>(plimits->add(COLLADA_ELEMENT_MIN))->getValue() = lmin;
                daeSafeCast<domMinmax>(plimits->add(COLLADA_ELEMENT_MAX))->getValue() = lmax;
            }
            vdomjoints.at(index) = pdomjoint;
        }

        LINKOUTPUT childinfo = _WriteLink(_robot.getRoot(), ktec, pnoderoot, kmodel->getID());
        FOREACHC(itused, childinfo.listusedlinks) {
            kmout->vlinksids.at(itused->first) = itused->second;
        }
        FOREACH(itprocessed,childinfo.listprocesseddofs) {
            kmout->vaxissids.at(itprocessed->first).jointnodesid = itprocessed->second;
        }
        kmout->_maplinkposes = childinfo._maplinkposes;

        // create the formulas for all mimic joints
        FOREACHC(itjoint, _robot.joints_) {
            string jointsid = _ComputeId(itjoint->second->name);
            boost::shared_ptr<urdf::Joint> pjoint = itjoint->second;
            if( !pjoint->mimic ) {
                continue;
            }

            domFormulaRef pf = daeSafeCast<domFormula>(ktec->add(COLLADA_ELEMENT_FORMULA));
            string formulaid = _ComputeId(str(boost::format("%s_formula")%jointsid));
            pf->setSid(formulaid.c_str());
            domCommon_float_or_paramRef ptarget = daeSafeCast<domCommon_float_or_param>(pf->add(COLLADA_ELEMENT_TARGET));
            string targetjointid = str(boost::format("%s/%s")%kmodel->getID()%jointsid);
            daeSafeCast<domCommon_param>(ptarget->add(COLLADA_TYPE_PARAM))->setValue(targetjointid.c_str());

            domTechniqueRef pftec = daeSafeCast<domTechnique>(pf->add(COLLADA_ELEMENT_TECHNIQUE));
            pftec->setProfile("OpenRAVE");
            // save position equation
            {
                daeElementRef poselt = pftec->add("equation");
                poselt->setAttribute("type","position");
                // create a const0*joint+const1 formula
                // <apply> <plus/> <apply> <times/> <cn>a</cn> x </apply> <cn>b</cn> </apply>
                daeElementRef pmath_math = poselt->add("math");
                daeElementRef pmath_apply = pmath_math->add("apply");
                {
                    daeElementRef pmath_plus = pmath_apply->add("plus");
                    daeElementRef pmath_apply1 = pmath_apply->add("apply");
                    {
                        daeElementRef pmath_times = pmath_apply1->add("times");
                        daeElementRef pmath_const0 = pmath_apply1->add("cn");
                        pmath_const0->setCharData(str(boost::format("%f")%pjoint->mimic->multiplier));
                        daeElementRef pmath_symb = pmath_apply1->add("csymbol");
                        pmath_symb->setAttribute("encoding","COLLADA");
                        pmath_symb->setCharData(str(boost::format("%s/%s")%kmodel->getID()%_ComputeId(pjoint->mimic->joint_name)));
                    }
                    daeElementRef pmath_const1 = pmath_apply->add("cn");
                    pmath_const1->setCharData(str(boost::format("%f")%pjoint->mimic->offset));
                }
            }
            // save first partial derivative
            {
                daeElementRef derivelt = pftec->add("equation");
                derivelt->setAttribute("type","first_partial");
                derivelt->setAttribute("target",str(boost::format("%s/%s")%kmodel->getID()%_ComputeId(pjoint->mimic->joint_name)).c_str());
                daeElementRef pmath_const0 = derivelt->add("cn");
                pmath_const0->setCharData(str(boost::format("%f")%pjoint->mimic->multiplier));
            }
            // save second partial derivative
            {
                daeElementRef derivelt = pftec->add("equation");
                derivelt->setAttribute("type","second_partial");
                derivelt->setAttribute("target",str(boost::format("%s/%s")%kmodel->getID()%_ComputeId(pjoint->mimic->joint_name)).c_str());
                daeElementRef pmath_const0 = derivelt->add("cn");
                pmath_const0->setCharData("0");
            }

            {
                domFormula_techniqueRef pfcommontec = daeSafeCast<domFormula_technique>(pf->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
                // create a const0*joint+const1 formula
                // <apply> <plus/> <apply> <times/> <cn>a</cn> x </apply> <cn>b</cn> </apply>
                daeElementRef pmath_math = pfcommontec->add("math");
                daeElementRef pmath_apply = pmath_math->add("apply");
                {
                    daeElementRef pmath_plus = pmath_apply->add("plus");
                    daeElementRef pmath_apply1 = pmath_apply->add("apply");
                    {
                        daeElementRef pmath_times = pmath_apply1->add("times");
                        daeElementRef pmath_const0 = pmath_apply1->add("cn");
                        pmath_const0->setCharData(str(boost::format("%f")%pjoint->mimic->multiplier));
                        daeElementRef pmath_symb = pmath_apply1->add("csymbol");
                        pmath_symb->setAttribute("encoding","COLLADA");
                        pmath_symb->setCharData(str(boost::format("%s/%s")%kmodel->getID()%_ComputeId(pjoint->mimic->joint_name)));
                    }
                    daeElementRef pmath_const1 = pmath_apply->add("cn");
                    pmath_const1->setCharData(str(boost::format("%f")%pjoint->mimic->offset));
                }
            }
        }

        return kmout;
    }

    /// \brief Write link of a kinematic body
    ///
    /// \param link Link to write
    /// \param pkinparent Kinbody parent
    /// \param pnodeparent Node parent
    /// \param strModelUri
    virtual LINKOUTPUT _WriteLink(boost::shared_ptr<const urdf::Link> plink, daeElementRef pkinparent, domNodeRef pnodeparent, const string& strModelUri)
    {
        LINKOUTPUT out;
        int linkindex = _maplinkindices[plink];
        string linksid = _ComputeId(plink->name);
        domLinkRef pdomlink = daeSafeCast<domLink>(pkinparent->add(COLLADA_ELEMENT_LINK));
        pdomlink->setName(plink->name.c_str());
        pdomlink->setSid(linksid.c_str());

        domNodeRef pnode = daeSafeCast<domNode>(pnodeparent->add(COLLADA_ELEMENT_NODE));
        string nodeid = _ComputeId(str(boost::format("v%s_node%d")%strModelUri%linkindex));
        pnode->setId( nodeid.c_str() );
        string nodesid = _ComputeId(str(boost::format("node%d")%linkindex));
        pnode->setSid(nodesid.c_str());
        pnode->setName(plink->name.c_str());

        boost::shared_ptr<urdf::Geometry> geometry;
        boost::shared_ptr<urdf::Material> material;
        urdf::Pose geometry_origin;
        if( !!plink->visual ) {
            geometry = plink->visual->geometry;
            material = plink->visual->material;
            geometry_origin = plink->visual->origin;
        }
        else if( !!plink->collision ) {
            geometry = plink->collision->geometry;
            geometry_origin = plink->collision->origin;
        }

        if( !!geometry ) {
            int igeom = 0;
            string geomid = _ComputeId(str(boost::format("g%s_%s_geom%d")%strModelUri%linksid%igeom));
            domGeometryRef pdomgeom = _WriteGeometry(geometry, geomid);
            domInstance_geometryRef pinstgeom = daeSafeCast<domInstance_geometry>(pnode->add(COLLADA_ELEMENT_INSTANCE_GEOMETRY));
            pinstgeom->setUrl((string("#")+geomid).c_str());

            // material
            _WriteMaterial(pdomgeom->getID(), material);
            domBind_materialRef pmat = daeSafeCast<domBind_material>(pinstgeom->add(COLLADA_ELEMENT_BIND_MATERIAL));
            domBind_material::domTechnique_commonRef pmattec = daeSafeCast<domBind_material::domTechnique_common>(pmat->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
            domInstance_materialRef pinstmat = daeSafeCast<domInstance_material>(pmattec->add(COLLADA_ELEMENT_INSTANCE_MATERIAL));
            pinstmat->setTarget(xsAnyURI(*pdomgeom, string("#")+geomid+string("_mat")));
            pinstmat->setSymbol("mat0");
        }

        _WriteTransformation(pnode, geometry_origin);
        urdf::Pose geometry_origin_inv = _poseInverse(geometry_origin);

        // process all children
        FOREACHC(itjoint, plink->child_joints) {
            boost::shared_ptr<urdf::Joint> pjoint = *itjoint;
            int index = _mapjointindices[pjoint];

            // <attachment_full joint="k1/joint0">
            domLink::domAttachment_fullRef attachment_full = daeSafeCast<domLink::domAttachment_full>(pdomlink->add(COLLADA_ELEMENT_ATTACHMENT_FULL));
            string jointid = str(boost::format("%s/%s")%strModelUri%_ComputeId(pjoint->name));
            attachment_full->setJoint(jointid.c_str());

            LINKOUTPUT childinfo = _WriteLink(_robot.getLink(pjoint->child_link_name), attachment_full, pnode, strModelUri);
            FOREACH(itused,childinfo.listusedlinks) {
                out.listusedlinks.push_back(make_pair(itused->first,linksid+string("/")+itused->second));
            }
            FOREACH(itprocessed,childinfo.listprocesseddofs) {
                out.listprocesseddofs.push_back(make_pair(itprocessed->first,nodesid+string("/")+itprocessed->second));
            }
            FOREACH(itlinkpos,childinfo._maplinkposes) {
                out._maplinkposes[itlinkpos->first] = _poseMult(pjoint->parent_to_joint_origin_transform,itlinkpos->second);
            }
            // rotate/translate elements
            string jointnodesid = _ComputeId(str(boost::format("node_%s_axis0")%pjoint->name));
            switch(pjoint->type) {
            case urdf::Joint::REVOLUTE:
            case urdf::Joint::CONTINUOUS:
            case urdf::Joint::FIXED: {
                domRotateRef protate = daeSafeCast<domRotate>(childinfo.pnode->add(COLLADA_ELEMENT_ROTATE,0));
                protate->setSid(jointnodesid.c_str());
                protate->getValue().setCount(4);
                protate->getValue()[0] = pjoint->axis.x;
                protate->getValue()[1] = pjoint->axis.y;
                protate->getValue()[2] = pjoint->axis.z;
                protate->getValue()[3] = 0;
                break;
            }
            case urdf::Joint::PRISMATIC: {
                domTranslateRef ptrans = daeSafeCast<domTranslate>(childinfo.pnode->add(COLLADA_ELEMENT_TRANSLATE,0));
                ptrans->setSid(jointnodesid.c_str());
                ptrans->getValue().setCount(3);
                ptrans->getValue()[0] = 0;
                ptrans->getValue()[1] = 0;
                ptrans->getValue()[2] = 0;
                break;
            }
            default:
                ROS_WARN_STREAM(str(boost::format("unsupported joint type specified %d")%(int)pjoint->type));
                break;
            }

            _WriteTransformation(attachment_full, pjoint->parent_to_joint_origin_transform);
            _WriteTransformation(childinfo.pnode, pjoint->parent_to_joint_origin_transform);
            _WriteTransformation(childinfo.pnode, geometry_origin_inv); // have to do multiply by inverse since geometry transformation is not part of hierarchy
            out.listprocesseddofs.push_back(make_pair(index,string(childinfo.pnode->getSid())+string("/")+jointnodesid));
            // </attachment_full>
        }

        out._maplinkposes[plink] = urdf::Pose();
        out.listusedlinks.push_back(make_pair(linkindex,linksid));
        out.plink = pdomlink;
        out.pnode = pnode;
        return out;
    }

    domGeometryRef _WriteGeometry(boost::shared_ptr<urdf::Geometry> geometry, const std::string& geometry_id)
    {
        domGeometryRef cgeometry = daeSafeCast<domGeometry>(_geometriesLib->add(COLLADA_ELEMENT_GEOMETRY));
        cgeometry->setId(geometry_id.c_str());
        switch (geometry->type) {
        case urdf::Geometry::MESH: {
            urdf::Mesh* urdf_mesh = (urdf::Mesh*) geometry.get();
            _loadMesh(urdf_mesh->filename, cgeometry, urdf_mesh->scale);
            break;
        }
        case urdf::Geometry::SPHERE: {
            urdf::Sphere* urdf_sphere = (urdf::Sphere *) geometry.get();
            double r = urdf_sphere->radius;

            std::vector<Point> sphere_vertices;

            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.309, r* 0.500, r* 0.809), urdf::Vector3(r* 0.809, r* 0.309, r* 0.500), urdf::Vector3(r* 0.500, r* 0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r* 0.526, r* 0.000), urdf::Vector3(r* 0.500, r* 0.809, r* 0.309), urdf::Vector3(r* 0.809, r* 0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r* 0.851), urdf::Vector3(r* 0.809, r* 0.309, r* 0.500), urdf::Vector3(r* 0.309, r* 0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r* 0.526), urdf::Vector3(r* 0.309, r* 0.500, r* 0.809), urdf::Vector3(r* 0.500, r* 0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.000, r* 1.000), urdf::Vector3(r*-0.309, r*-0.500, r* 0.809), urdf::Vector3(r* 0.309, r*-0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r* 0.526), urdf::Vector3(r* 0.309, r*-0.500, r* 0.809), urdf::Vector3(r*-0.309, r*-0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r* 0.851), urdf::Vector3(r*-0.309, r*-0.500, r* 0.809), urdf::Vector3(r* 0.000, r* 0.000, r* 1.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r* 0.851), urdf::Vector3(r* 0.000, r* 0.000, r* 1.000), urdf::Vector3(r* 0.309, r*-0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.500, r* 0.809, r* 0.309), urdf::Vector3(r* 0.000, r* 1.000, r* 0.000), urdf::Vector3(r*-0.500, r* 0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r*-0.526), urdf::Vector3(r*-0.500, r* 0.809, r*-0.309), urdf::Vector3(r* 0.000, r* 1.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r* 0.526), urdf::Vector3(r* 0.000, r* 1.000, r* 0.000), urdf::Vector3(r*-0.500, r* 0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r* 0.526, r* 0.000), urdf::Vector3(r*-0.500, r* 0.809, r* 0.309), urdf::Vector3(r*-0.500, r* 0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.309, r* 0.500, r* 0.809), urdf::Vector3(r* 0.000, r* 0.000, r* 1.000), urdf::Vector3(r* 0.309, r* 0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r* 0.851), urdf::Vector3(r* 0.309, r* 0.500, r* 0.809), urdf::Vector3(r* 0.000, r* 0.000, r* 1.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r* 0.851), urdf::Vector3(r* 0.000, r* 0.000, r* 1.000), urdf::Vector3(r*-0.309, r* 0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r* 0.526), urdf::Vector3(r*-0.309, r* 0.500, r* 0.809), urdf::Vector3(r* 0.309, r* 0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.809, r* 0.309, r* 0.500), urdf::Vector3(r*-0.309, r* 0.500, r* 0.809), urdf::Vector3(r*-0.500, r* 0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r* 0.526), urdf::Vector3(r*-0.500, r* 0.809, r* 0.309), urdf::Vector3(r*-0.309, r* 0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r* 0.851), urdf::Vector3(r*-0.309, r* 0.500, r* 0.809), urdf::Vector3(r*-0.809, r* 0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r* 0.526, r* 0.000), urdf::Vector3(r*-0.809, r* 0.309, r* 0.500), urdf::Vector3(r*-0.500, r* 0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-1.000, r* 0.000, r* 0.000), urdf::Vector3(r*-0.809, r*-0.309, r* 0.500), urdf::Vector3(r*-0.809, r* 0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r* 0.851), urdf::Vector3(r*-0.809, r* 0.309, r* 0.500), urdf::Vector3(r*-0.809, r*-0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r*-0.526, r* 0.000), urdf::Vector3(r*-0.809, r*-0.309, r* 0.500), urdf::Vector3(r*-1.000, r* 0.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r* 0.526, r* 0.000), urdf::Vector3(r*-1.000, r* 0.000, r* 0.000), urdf::Vector3(r*-0.809, r* 0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.809, r*-0.309, r*-0.500), urdf::Vector3(r*-1.000, r* 0.000, r* 0.000), urdf::Vector3(r*-0.809, r* 0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r* 0.526, r* 0.000), urdf::Vector3(r*-0.809, r* 0.309, r*-0.500), urdf::Vector3(r*-1.000, r* 0.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r*-0.526, r* 0.000), urdf::Vector3(r*-1.000, r* 0.000, r* 0.000), urdf::Vector3(r*-0.809, r*-0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r*-0.851), urdf::Vector3(r*-0.809, r*-0.309, r*-0.500), urdf::Vector3(r*-0.809, r* 0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.500, r*-0.809, r*-0.309), urdf::Vector3(r*-0.809, r*-0.309, r*-0.500), urdf::Vector3(r*-0.309, r*-0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r*-0.851), urdf::Vector3(r*-0.309, r*-0.500, r*-0.809), urdf::Vector3(r*-0.809, r*-0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r*-0.526, r* 0.000), urdf::Vector3(r*-0.809, r*-0.309, r*-0.500), urdf::Vector3(r*-0.500, r*-0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r*-0.526), urdf::Vector3(r*-0.500, r*-0.809, r*-0.309), urdf::Vector3(r*-0.309, r*-0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.500, r*-0.809, r* 0.309), urdf::Vector3(r*-0.309, r*-0.500, r* 0.809), urdf::Vector3(r*-0.809, r*-0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r* 0.851), urdf::Vector3(r*-0.809, r*-0.309, r* 0.500), urdf::Vector3(r*-0.309, r*-0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r* 0.526), urdf::Vector3(r*-0.309, r*-0.500, r* 0.809), urdf::Vector3(r*-0.500, r*-0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r*-0.526, r* 0.000), urdf::Vector3(r*-0.500, r*-0.809, r* 0.309), urdf::Vector3(r*-0.809, r*-0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-1.000, r* 0.000), urdf::Vector3(r*-0.500, r*-0.809, r* 0.309), urdf::Vector3(r*-0.500, r*-0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r*-0.526, r* 0.000), urdf::Vector3(r*-0.500, r*-0.809, r*-0.309), urdf::Vector3(r*-0.500, r*-0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r* 0.526), urdf::Vector3(r*-0.500, r*-0.809, r* 0.309), urdf::Vector3(r* 0.000, r*-1.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r*-0.526), urdf::Vector3(r* 0.000, r*-1.000, r* 0.000), urdf::Vector3(r*-0.500, r*-0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.500, r*-0.809, r* 0.309), urdf::Vector3(r* 0.809, r*-0.309, r* 0.500), urdf::Vector3(r* 0.309, r*-0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r* 0.851), urdf::Vector3(r* 0.309, r*-0.500, r* 0.809), urdf::Vector3(r* 0.809, r*-0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r*-0.526, r* 0.000), urdf::Vector3(r* 0.809, r*-0.309, r* 0.500), urdf::Vector3(r* 0.500, r*-0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r* 0.526), urdf::Vector3(r* 0.500, r*-0.809, r* 0.309), urdf::Vector3(r* 0.309, r*-0.500, r* 0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.500, r*-0.809, r*-0.309), urdf::Vector3(r* 0.500, r*-0.809, r* 0.309), urdf::Vector3(r* 0.000, r*-1.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r* 0.526), urdf::Vector3(r* 0.000, r*-1.000, r* 0.000), urdf::Vector3(r* 0.500, r*-0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r*-0.526, r* 0.000), urdf::Vector3(r* 0.500, r*-0.809, r* 0.309), urdf::Vector3(r* 0.500, r*-0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r*-0.526), urdf::Vector3(r* 0.500, r*-0.809, r*-0.309), urdf::Vector3(r* 0.000, r*-1.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.809, r*-0.309, r*-0.500), urdf::Vector3(r* 0.500, r*-0.809, r*-0.309), urdf::Vector3(r* 0.309, r*-0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r*-0.526), urdf::Vector3(r* 0.309, r*-0.500, r*-0.809), urdf::Vector3(r* 0.500, r*-0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r*-0.526, r* 0.000), urdf::Vector3(r* 0.500, r*-0.809, r*-0.309), urdf::Vector3(r* 0.809, r*-0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r*-0.851), urdf::Vector3(r* 0.809, r*-0.309, r*-0.500), urdf::Vector3(r* 0.309, r*-0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 1.000, r* 0.000, r* 0.000), urdf::Vector3(r* 0.809, r* 0.309, r* 0.500), urdf::Vector3(r* 0.809, r*-0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r* 0.851), urdf::Vector3(r* 0.809, r*-0.309, r* 0.500), urdf::Vector3(r* 0.809, r* 0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r* 0.526, r* 0.000), urdf::Vector3(r* 0.809, r* 0.309, r* 0.500), urdf::Vector3(r* 1.000, r* 0.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r*-0.526, r* 0.000), urdf::Vector3(r* 1.000, r* 0.000, r* 0.000), urdf::Vector3(r* 0.809, r*-0.309, r* 0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.809, r* 0.309, r*-0.500), urdf::Vector3(r* 1.000, r* 0.000, r* 0.000), urdf::Vector3(r* 0.809, r*-0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r*-0.526, r* 0.000), urdf::Vector3(r* 0.809, r*-0.309, r*-0.500), urdf::Vector3(r* 1.000, r* 0.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r* 0.526, r* 0.000), urdf::Vector3(r* 1.000, r* 0.000, r* 0.000), urdf::Vector3(r* 0.809, r* 0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r*-0.851), urdf::Vector3(r* 0.809, r* 0.309, r*-0.500), urdf::Vector3(r* 0.809, r*-0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.500, r* 0.809, r*-0.309), urdf::Vector3(r* 0.000, r* 1.000, r* 0.000), urdf::Vector3(r* 0.500, r* 0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r* 0.526), urdf::Vector3(r* 0.500, r* 0.809, r* 0.309), urdf::Vector3(r* 0.000, r* 1.000, r* 0.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r*-0.526), urdf::Vector3(r* 0.000, r* 1.000, r* 0.000), urdf::Vector3(r* 0.500, r* 0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r* 0.526, r* 0.000), urdf::Vector3(r* 0.500, r* 0.809, r*-0.309), urdf::Vector3(r* 0.500, r* 0.809, r* 0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.309, r* 0.500, r*-0.809), urdf::Vector3(r* 0.500, r* 0.809, r*-0.309), urdf::Vector3(r* 0.809, r* 0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.851, r* 0.526, r* 0.000), urdf::Vector3(r* 0.809, r* 0.309, r*-0.500), urdf::Vector3(r* 0.500, r* 0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r*-0.526), urdf::Vector3(r* 0.500, r* 0.809, r*-0.309), urdf::Vector3(r* 0.309, r* 0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r*-0.851), urdf::Vector3(r* 0.309, r* 0.500, r*-0.809), urdf::Vector3(r* 0.809, r* 0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.309, r* 0.500, r*-0.809), urdf::Vector3(r*-0.809, r* 0.309, r*-0.500), urdf::Vector3(r*-0.500, r* 0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.851, r* 0.526, r* 0.000), urdf::Vector3(r*-0.500, r* 0.809, r*-0.309), urdf::Vector3(r*-0.809, r* 0.309, r*-0.500)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r*-0.851), urdf::Vector3(r*-0.809, r* 0.309, r*-0.500), urdf::Vector3(r*-0.309, r* 0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r*-0.526), urdf::Vector3(r*-0.309, r* 0.500, r*-0.809), urdf::Vector3(r*-0.500, r* 0.809, r*-0.309)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.000, r*-1.000), urdf::Vector3(r* 0.309, r*-0.500, r*-0.809), urdf::Vector3(r*-0.309, r*-0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r*-0.851, r*-0.526), urdf::Vector3(r*-0.309, r*-0.500, r*-0.809), urdf::Vector3(r* 0.309, r*-0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r*-0.851), urdf::Vector3(r* 0.309, r*-0.500, r*-0.809), urdf::Vector3(r* 0.000, r* 0.000, r*-1.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r*-0.851), urdf::Vector3(r* 0.000, r* 0.000, r*-1.000), urdf::Vector3(r*-0.309, r*-0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.000, r*-1.000), urdf::Vector3(r*-0.309, r* 0.500, r*-0.809), urdf::Vector3(r* 0.309, r* 0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.000, r* 0.851, r*-0.526), urdf::Vector3(r* 0.309, r* 0.500, r*-0.809), urdf::Vector3(r*-0.309, r* 0.500, r*-0.809)));
            sphere_vertices.push_back(Point(urdf::Vector3(r*-0.526, r* 0.000, r*-0.851), urdf::Vector3(r*-0.309, r* 0.500, r*-0.809), urdf::Vector3(r* 0.000, r* 0.000, r*-1.000)));
            sphere_vertices.push_back(Point(urdf::Vector3(r* 0.526, r* 0.000, r*-0.851), urdf::Vector3(r* 0.000, r* 0.000, r*-1.000), urdf::Vector3(r* 0.309, r* 0.500, r*-0.809)));

            _loadVertices(sphere_vertices, cgeometry);
            break;
        }
        case urdf::Geometry::BOX: {
            urdf::Box* urdf_box = (urdf::Box*) geometry.get();
            double x = urdf_box->dim.x/2;
            double y = urdf_box->dim.y/2;
            double z = urdf_box->dim.z/2;
            std::vector<Point> box_vertices;
            box_vertices.push_back(Point(urdf::Vector3( x,  y, -z), urdf::Vector3(-x, -y, -z), urdf::Vector3(-x,  y, -z)));
            box_vertices.push_back(Point(urdf::Vector3(-x, -y, -z), urdf::Vector3( x,  y, -z), urdf::Vector3( x, -y, -z)));
            box_vertices.push_back(Point(urdf::Vector3(-x,  y,  z), urdf::Vector3( x, -y,  z), urdf::Vector3( x,  y,  z)));
            box_vertices.push_back(Point(urdf::Vector3( x, -y,  z), urdf::Vector3(-x,  y,  z), urdf::Vector3(-x, -y,  z)));

            box_vertices.push_back(Point(urdf::Vector3( x,  y, -z), urdf::Vector3(-x,  y,  z), urdf::Vector3( x,  y,  z)));
            box_vertices.push_back(Point(urdf::Vector3(-x,  y,  z), urdf::Vector3( x,  y, -z), urdf::Vector3(-x,  y, -z)));
            box_vertices.push_back(Point(urdf::Vector3( x, -y, -z), urdf::Vector3( x,  y,  z), urdf::Vector3( x, -y,  z)));
            box_vertices.push_back(Point(urdf::Vector3( x,  y,  z), urdf::Vector3( x, -y, -z), urdf::Vector3( x,  y, -z)));

            box_vertices.push_back(Point(urdf::Vector3(-x, -y, -z), urdf::Vector3( x, -y,  z), urdf::Vector3(-x, -y,  z)));
            box_vertices.push_back(Point(urdf::Vector3( x, -y,  z), urdf::Vector3(-x, -y, -z), urdf::Vector3( x, -y, -z)));
            box_vertices.push_back(Point(urdf::Vector3(-x,  y, -z), urdf::Vector3(-x, -y,  z), urdf::Vector3(-x,  y,  z)));
            box_vertices.push_back(Point(urdf::Vector3(-x, -y,  z), urdf::Vector3(-x,  y, -z), urdf::Vector3(-x, -y, -z)));

            _loadVertices(box_vertices, cgeometry);
            break;
        }
        case urdf::Geometry::CYLINDER: {
            urdf::Cylinder* urdf_cylinder = (urdf::Cylinder*) geometry.get();
            double l = urdf_cylinder->length;
            double r = urdf_cylinder->radius;
            std::vector<Point> cylinder_vertices;
            for(int i = 0; i < 32; i++ ) {
                double s1 = sin(2*M_PI*i/32);
                double c1 = cos(2*M_PI*i/32);
                double s2 = sin(2*M_PI*(i+1)/32);
                double c2 = cos(2*M_PI*(i+1)/32);
                cylinder_vertices.push_back(Point(urdf::Vector3(0, 0,-l/2), urdf::Vector3(r*s2, r*c2,-l/2), urdf::Vector3(r*s1, r*c1,-l/2)));
                cylinder_vertices.push_back(Point(urdf::Vector3(r*s1, r*c1,-l/2), urdf::Vector3(r*s2, r*c2,-l/2), urdf::Vector3(r*s2, r*c2, l/2)));
                cylinder_vertices.push_back(Point(urdf::Vector3(r*s1, r*c1,-l/2), urdf::Vector3(r*s2, r*c2, l/2), urdf::Vector3(r*s1, r*c1, l/2)));
                cylinder_vertices.push_back(Point(urdf::Vector3(0, 0, l/2), urdf::Vector3(r*s1, r*c1, l/2), urdf::Vector3(r*s2, r*c2, l/2)));
            }
            _loadVertices(cylinder_vertices, cgeometry);

            break;
        }
        default: {
            throw ColladaUrdfException(str(boost::format("undefined geometry type %d, name %s")%(int)geometry->type%geometry_id));
        }
        }
        return cgeometry;
    }

    void _WriteMaterial(const string& geometry_id, boost::shared_ptr<urdf::Material> material)
    {
        string effid = geometry_id+string("_eff");
        string matid = geometry_id+string("_mat");
        domMaterialRef pdommat = daeSafeCast<domMaterial>(_materialsLib->add(COLLADA_ELEMENT_MATERIAL));
        pdommat->setId(matid.c_str());
        domInstance_effectRef pdominsteff = daeSafeCast<domInstance_effect>(pdommat->add(COLLADA_ELEMENT_INSTANCE_EFFECT));
        pdominsteff->setUrl((string("#")+effid).c_str());

        urdf::Color ambient, diffuse;
        ambient.init("0.1 0.1 0.1 0");
        diffuse.init("1 1 1 0");

        if( !!material ) {
            ambient.r = diffuse.r = material->color.r;
            ambient.g = diffuse.g = material->color.g;
            ambient.b = diffuse.b = material->color.b;
            ambient.a = diffuse.a = material->color.a;
        }

        domEffectRef effect = _WriteEffect(geometry_id, ambient, diffuse);

        // <material id="g1.link0.geom0.eff">
        domMaterialRef dommaterial = daeSafeCast<domMaterial>(_materialsLib->add(COLLADA_ELEMENT_MATERIAL));
        string material_id = geometry_id + string("_mat");
        dommaterial->setId(material_id.c_str());
        {
            // <instance_effect url="#g1.link0.geom0.eff"/>
            domInstance_effectRef instance_effect = daeSafeCast<domInstance_effect>(dommaterial->add(COLLADA_ELEMENT_INSTANCE_EFFECT));
            string effect_id(effect->getId());
            instance_effect->setUrl((string("#") + effect_id).c_str());
        }
        // </material>

        domEffectRef pdomeff = _WriteEffect(effid, ambient, diffuse);
    }

    boost::shared_ptr<instance_physics_model_output> _WriteInstance_physics_model(int id, daeElementRef parent, const string& sidscope, const MAPLINKPOSES& maplinkposes)
    {
        boost::shared_ptr<physics_model_output> pmout = WritePhysics_model(id, maplinkposes);
        boost::shared_ptr<instance_physics_model_output> ipmout(new instance_physics_model_output());
        ipmout->pmout = pmout;
        ipmout->ipm = daeSafeCast<domInstance_physics_model>(parent->add(COLLADA_ELEMENT_INSTANCE_PHYSICS_MODEL));
        string bodyid = _ComputeId(str(boost::format("visual%d")%id));
        ipmout->ipm->setParent(xsAnyURI(*ipmout->ipm,string("#")+bodyid));
        string symscope, refscope;
        if( sidscope.size() > 0 ) {
            symscope = sidscope+string("_");
            refscope = sidscope+string("/");
        }
        string ipmsid = str(boost::format("%s_inst")%pmout->pmodel->getID());
        ipmout->ipm->setUrl(str(boost::format("#%s")%pmout->pmodel->getID()).c_str());
        ipmout->ipm->setSid(ipmsid.c_str());

        string kmodelid = _ComputeKinematics_modelId(id);
        for(size_t i = 0; i < pmout->vrigidbodysids.size(); ++i) {
            domInstance_rigid_bodyRef pirb = daeSafeCast<domInstance_rigid_body>(ipmout->ipm->add(COLLADA_ELEMENT_INSTANCE_RIGID_BODY));
            pirb->setBody(pmout->vrigidbodysids[i].c_str());
            pirb->setTarget(xsAnyURI(*pirb,str(boost::format("#v%s_node%d")%kmodelid%i)));
        }

        return ipmout;
    }

    boost::shared_ptr<physics_model_output> WritePhysics_model(int id, const MAPLINKPOSES& maplinkposes)
    {
        boost::shared_ptr<physics_model_output> pmout(new physics_model_output());
        pmout->pmodel = daeSafeCast<domPhysics_model>(_physicsModelsLib->add(COLLADA_ELEMENT_PHYSICS_MODEL));
        string pmodelid = str(boost::format("pmodel%d")%id);
        pmout->pmodel->setId(pmodelid.c_str());
        pmout->pmodel->setName(_robot.getName().c_str());
        FOREACHC(itlink,_robot.links_) {
            domRigid_bodyRef rigid_body = daeSafeCast<domRigid_body>(pmout->pmodel->add(COLLADA_ELEMENT_RIGID_BODY));
            string rigidsid = str(boost::format("rigid%d")%_maplinkindices[itlink->second]);
            pmout->vrigidbodysids.push_back(rigidsid);
            rigid_body->setSid(rigidsid.c_str());
            rigid_body->setName(itlink->second->name.c_str());
            domRigid_body::domTechnique_commonRef ptec = daeSafeCast<domRigid_body::domTechnique_common>(rigid_body->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
            boost::shared_ptr<urdf::Inertial> inertial = itlink->second->inertial;
            if( !!inertial ) {
                daeSafeCast<domRigid_body::domTechnique_common::domDynamic>(ptec->add(COLLADA_ELEMENT_DYNAMIC))->setValue(xsBoolean(true)); //!!inertial));
                domTargetable_floatRef mass = daeSafeCast<domTargetable_float>(ptec->add(COLLADA_ELEMENT_MASS));
                mass->setValue(inertial->mass);
                double fCovariance[9] = { inertial->ixx, inertial->ixy, inertial->ixz, inertial->ixy, inertial->iyy, inertial->iyz, inertial->ixz, inertial->iyz, inertial->izz};
                double eigenvalues[3], eigenvectors[9];
                mathextra::EigenSymmetric3(fCovariance,eigenvalues,eigenvectors);
                boost::array<double,12> minertiaframe;
                for(int j = 0; j < 3; ++j) {
                    minertiaframe[4*0+j] = eigenvectors[3*j];
                    minertiaframe[4*1+j] = eigenvectors[3*j+1];
                    minertiaframe[4*2+j] = eigenvectors[3*j+2];
                }
                urdf::Pose tinertiaframe;
                tinertiaframe.rotation = _quatFromMatrix(minertiaframe);
                tinertiaframe = _poseMult(inertial->origin,tinertiaframe);

                domTargetable_float3Ref pinertia = daeSafeCast<domTargetable_float3>(ptec->add(COLLADA_ELEMENT_INERTIA));
                pinertia->getValue().setCount(3);
                pinertia->getValue()[0] = eigenvalues[0];
                pinertia->getValue()[1] = eigenvalues[1];
                pinertia->getValue()[2] = eigenvalues[2];
                urdf::Pose posemassframe = _poseMult(maplinkposes.find(itlink->second)->second,tinertiaframe);
                _WriteTransformation(ptec->add(COLLADA_ELEMENT_MASS_FRAME), posemassframe);

                //            // create a shape for every geometry
                //            int igeom = 0;
                //            FOREACHC(itgeom, (*itlink)->GetGeometries()) {
                //                domRigid_body::domTechnique_common::domShapeRef pdomshape = daeSafeCast<domRigid_body::domTechnique_common::domShape>(ptec->add(COLLADA_ELEMENT_SHAPE));
                //                // there is a weird bug here where _WriteTranformation will fail to create rotate/translate elements in instance_geometry is created first... (is this part of the spec?)
                //                _WriteTransformation(pdomshape,tbaseinv*(*itlink)->GetTransform()*itgeom->GetTransform());
                //                domInstance_geometryRef pinstgeom = daeSafeCast<domInstance_geometry>(pdomshape->add(COLLADA_ELEMENT_INSTANCE_GEOMETRY));
                //                pinstgeom->setUrl(xsAnyURI(*pinstgeom,string("#")+_GetGeometryId(*itlink,igeom)));
                //                ++igeom;
                //            }
            }
        }
        return pmout;
    }

    void _loadVertices(std::vector<Point> vertices, domGeometryRef pdomgeom) {
            aiScene* scene = new aiScene();
            scene->mRootNode = new aiNode();
            scene->mRootNode->mNumMeshes = 1;
            scene->mRootNode->mMeshes  = (unsigned int*)malloc(sizeof(unsigned int));
            scene->mRootNode->mMeshes[0] = 0;
            scene->mNumMeshes = 1;
            scene->mMeshes = (aiMesh **)malloc(sizeof(aiMesh*));
            scene->mMeshes[0] = new aiMesh();
            scene->mMeshes[0]->mNumFaces = 0;
            scene->mMeshes[0]->mFaces = (aiFace *)malloc(sizeof(aiFace)*vertices.size());
            scene->mMeshes[0]->mNumVertices = 0;
            scene->mMeshes[0]->mVertices = (aiVector3D *)malloc(sizeof(aiVector3D)*vertices.size()*3);

            FOREACH(it, vertices) {
                scene->mMeshes[0]->mFaces[scene->mMeshes[0]->mNumFaces].mNumIndices = 3;
                scene->mMeshes[0]->mFaces[scene->mMeshes[0]->mNumFaces].mIndices = (unsigned int *)malloc(sizeof(unsigned int)*3);

                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].x = it->p1.x;
                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].y = it->p1.y;
                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].z = it->p1.z;
                scene->mMeshes[0]->mFaces[scene->mMeshes[0]->mNumFaces].mIndices[0] = scene->mMeshes[0]->mNumVertices;
                scene->mMeshes[0]->mNumVertices++;

                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].x = it->p2.x;
                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].y = it->p2.y;
                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].z = it->p2.z;
                scene->mMeshes[0]->mFaces[scene->mMeshes[0]->mNumFaces].mIndices[1] = scene->mMeshes[0]->mNumVertices;
                scene->mMeshes[0]->mNumVertices++;

                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].x = it->p3.x;
                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].y = it->p3.y;
                scene->mMeshes[0]->mVertices[scene->mMeshes[0]->mNumVertices].z = it->p3.z;
                scene->mMeshes[0]->mFaces[scene->mMeshes[0]->mNumFaces].mIndices[2] = scene->mMeshes[0]->mNumVertices;
                scene->mMeshes[0]->mNumVertices++;

                scene->mMeshes[0]->mNumFaces++;
            }

            domMeshRef pdommesh = daeSafeCast<domMesh>(pdomgeom->add(COLLADA_ELEMENT_MESH));
            domSourceRef pvertsource = daeSafeCast<domSource>(pdommesh->add(COLLADA_ELEMENT_SOURCE));
            domAccessorRef pacc;
            domFloat_arrayRef parray;
            {
                pvertsource->setId(str(boost::format("%s_positions")%pdomgeom->getID()).c_str());

                parray = daeSafeCast<domFloat_array>(pvertsource->add(COLLADA_ELEMENT_FLOAT_ARRAY));
                parray->setId(str(boost::format("%s_positions-array")%pdomgeom->getID()).c_str());
                parray->setDigits(6); // 6 decimal places

                domSource::domTechnique_commonRef psourcetec = daeSafeCast<domSource::domTechnique_common>(pvertsource->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
                pacc = daeSafeCast<domAccessor>(psourcetec->add(COLLADA_ELEMENT_ACCESSOR));
                pacc->setSource(xsAnyURI(*parray, std::string("#")+string(parray->getID())));
                pacc->setStride(3);

                domParamRef px = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
                px->setName("X"); px->setType("float");
                domParamRef py = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
                py->setName("Y"); py->setType("float");
                domParamRef pz = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
                pz->setName("Z"); pz->setType("float");
            }
            domVerticesRef pverts = daeSafeCast<domVertices>(pdommesh->add(COLLADA_ELEMENT_VERTICES));
            {
                pverts->setId("vertices");
                domInput_localRef pvertinput = daeSafeCast<domInput_local>(pverts->add(COLLADA_ELEMENT_INPUT));
                pvertinput->setSemantic("POSITION");
                pvertinput->setSource(domUrifragment(*pvertsource, std::string("#")+std::string(pvertsource->getID())));
            }
            _buildAiMesh(scene,scene->mRootNode,pdommesh,parray, pdomgeom->getID(), urdf::Vector3(1,1,1));
            pacc->setCount(parray->getCount());
    }
    void _loadMesh(std::string const& filename, domGeometryRef pdomgeom, const urdf::Vector3& scale)
    {
        const aiScene* scene = _importer.ReadFile(filename, aiProcess_SortByPType|aiProcess_Triangulate); //|aiProcess_GenNormals|aiProcess_GenUVCoords|aiProcess_FlipUVs);
        if( !scene ) {
            ROS_WARN("failed to load resource %s",filename.c_str());
            return;
        }
        if( !scene->mRootNode ) {
            ROS_WARN("resource %s has no data",filename.c_str());
            return;
        }
        if (!scene->HasMeshes()) {
            ROS_WARN_STREAM(str(boost::format("No meshes found in file %s")%filename));
            return;
        }
        domMeshRef pdommesh = daeSafeCast<domMesh>(pdomgeom->add(COLLADA_ELEMENT_MESH));
        domSourceRef pvertsource = daeSafeCast<domSource>(pdommesh->add(COLLADA_ELEMENT_SOURCE));
        domAccessorRef pacc;
        domFloat_arrayRef parray;
        {
            pvertsource->setId(str(boost::format("%s_positions")%pdomgeom->getID()).c_str());

            parray = daeSafeCast<domFloat_array>(pvertsource->add(COLLADA_ELEMENT_FLOAT_ARRAY));
            parray->setId(str(boost::format("%s_positions-array")%pdomgeom->getID()).c_str());
            parray->setDigits(6); // 6 decimal places

            domSource::domTechnique_commonRef psourcetec = daeSafeCast<domSource::domTechnique_common>(pvertsource->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
            pacc = daeSafeCast<domAccessor>(psourcetec->add(COLLADA_ELEMENT_ACCESSOR));
            pacc->setSource(xsAnyURI(*parray, string("#")+string(parray->getID())));
            pacc->setStride(3);

            domParamRef px = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
            px->setName("X"); px->setType("float");
            domParamRef py = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
            py->setName("Y"); py->setType("float");
            domParamRef pz = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
            pz->setName("Z"); pz->setType("float");
        }
        domVerticesRef pverts = daeSafeCast<domVertices>(pdommesh->add(COLLADA_ELEMENT_VERTICES));
        {
            pverts->setId("vertices");
            domInput_localRef pvertinput = daeSafeCast<domInput_local>(pverts->add(COLLADA_ELEMENT_INPUT));
            pvertinput->setSemantic("POSITION");
            pvertinput->setSource(domUrifragment(*pvertsource, string("#")+string(pvertsource->getID())));
        }
        _buildAiMesh(scene,scene->mRootNode,pdommesh,parray, pdomgeom->getID(),scale);
        pacc->setCount(parray->getCount());
    }

    void _buildAiMesh(const aiScene* scene, aiNode* node, domMeshRef pdommesh, domFloat_arrayRef parray, const string& geomid, const urdf::Vector3& scale)
    {
        if( !node ) {
            return;
        }
        aiMatrix4x4 transform = node->mTransformation;
        aiNode *pnode = node->mParent;
        while (pnode) {
            // Don't convert to y-up orientation, which is what the root node in
            // Assimp does
            if (pnode->mParent != NULL) {
                transform = pnode->mTransformation * transform;
            }
            pnode = pnode->mParent;
        }

        {
            uint32_t vertexOffset = parray->getCount();
            uint32_t nTotalVertices=0;
            for (uint32_t i = 0; i < node->mNumMeshes; i++) {
                aiMesh* input_mesh = scene->mMeshes[node->mMeshes[i]];
                nTotalVertices += input_mesh->mNumVertices;
            }

            parray->getValue().grow(parray->getCount()+nTotalVertices*3);
            parray->setCount(parray->getCount()+nTotalVertices);

            for (uint32_t i = 0; i < node->mNumMeshes; i++) {
                aiMesh* input_mesh = scene->mMeshes[node->mMeshes[i]];
                for (uint32_t j = 0; j < input_mesh->mNumVertices; j++) {
                    aiVector3D p = input_mesh->mVertices[j];
                    p *= transform;
                    parray->getValue().append(p.x*scale.x);
                    parray->getValue().append(p.y*scale.y);
                    parray->getValue().append(p.z*scale.z);
                }
            }

            // in order to save space, separate triangles from poly lists

            vector<int> triangleindices, otherindices;
            int nNumOtherPrimitives = 0;
            for (uint32_t i = 0; i < node->mNumMeshes; i++) {
                aiMesh* input_mesh = scene->mMeshes[node->mMeshes[i]];
                uint32_t indexCount = 0, otherIndexCount = 0;
                for (uint32_t j = 0; j < input_mesh->mNumFaces; j++) {
                    aiFace& face = input_mesh->mFaces[j];
                    if( face.mNumIndices == 3 ) {
                        indexCount += face.mNumIndices;
                    }
                    else {
                        otherIndexCount += face.mNumIndices;
                    }
                }
                triangleindices.reserve(triangleindices.size()+indexCount);
                otherindices.reserve(otherindices.size()+otherIndexCount);
                for (uint32_t j = 0; j < input_mesh->mNumFaces; j++) {
                    aiFace& face = input_mesh->mFaces[j];
                    if( face.mNumIndices == 3 ) {
                        triangleindices.push_back(vertexOffset+face.mIndices[0]);
                        triangleindices.push_back(vertexOffset+face.mIndices[1]);
                        triangleindices.push_back(vertexOffset+face.mIndices[2]);
                    }
                    else {
                        for (uint32_t k = 0; k < face.mNumIndices; ++k) {
                            otherindices.push_back(face.mIndices[k]+vertexOffset);
                        }
                        nNumOtherPrimitives++;
                    }
                }
                vertexOffset += input_mesh->mNumVertices;
            }

            if( triangleindices.size() > 0 ) {
                domTrianglesRef ptris = daeSafeCast<domTriangles>(pdommesh->add(COLLADA_ELEMENT_TRIANGLES));
                ptris->setCount(triangleindices.size()/3);
                ptris->setMaterial("mat0");
                domInput_local_offsetRef pvertoffset = daeSafeCast<domInput_local_offset>(ptris->add(COLLADA_ELEMENT_INPUT));
                pvertoffset->setSemantic("VERTEX");
                pvertoffset->setOffset(0);
                pvertoffset->setSource(domUrifragment(*pdommesh->getVertices(), str(boost::format("#%s/vertices")%geomid)));
                domPRef pindices = daeSafeCast<domP>(ptris->add(COLLADA_ELEMENT_P));
                pindices->getValue().setCount(triangleindices.size());
                for(size_t ind = 0; ind < triangleindices.size(); ++ind) {
                    pindices->getValue()[ind] = triangleindices[ind];
                }
            }

            if( nNumOtherPrimitives > 0 ) {
                domPolylistRef ptris = daeSafeCast<domPolylist>(pdommesh->add(COLLADA_ELEMENT_POLYLIST));
                ptris->setCount(nNumOtherPrimitives);
                ptris->setMaterial("mat0");
                domInput_local_offsetRef pvertoffset = daeSafeCast<domInput_local_offset>(ptris->add(COLLADA_ELEMENT_INPUT));
                pvertoffset->setSemantic("VERTEX");
                pvertoffset->setSource(domUrifragment(*pdommesh->getVertices(), str(boost::format("#%s/vertices")%geomid)));
                domPRef pindices = daeSafeCast<domP>(ptris->add(COLLADA_ELEMENT_P));
                pindices->getValue().setCount(otherindices.size());
                for(size_t ind = 0; ind < otherindices.size(); ++ind) {
                    pindices->getValue()[ind] = otherindices[ind];
                }

                domPolylist::domVcountRef pcount = daeSafeCast<domPolylist::domVcount>(ptris->add(COLLADA_ELEMENT_VCOUNT));
                pcount->getValue().setCount(nNumOtherPrimitives);
                uint32_t offset = 0;
                for (uint32_t i = 0; i < node->mNumMeshes; i++) {
                    aiMesh* input_mesh = scene->mMeshes[node->mMeshes[i]];
                    for (uint32_t j = 0; j < input_mesh->mNumFaces; j++) {
                        aiFace& face = input_mesh->mFaces[j];
                        if( face.mNumIndices > 3 ) {
                            pcount->getValue()[offset++] = face.mNumIndices;
                        }
                    }
                }
            }
        }

        for (uint32_t i=0; i < node->mNumChildren; ++i) {
            _buildAiMesh(scene, node->mChildren[i], pdommesh,parray,geomid,scale);
        }
    }


    domEffectRef _WriteEffect(std::string const& effect_id, urdf::Color const& color_ambient, urdf::Color const& color_diffuse)
    {
        // <effect id="g1.link0.geom0.eff">
        domEffectRef effect = daeSafeCast<domEffect>(_effectsLib->add(COLLADA_ELEMENT_EFFECT));
        effect->setId(effect_id.c_str());
        {
            // <profile_COMMON>
            domProfile_commonRef profile = daeSafeCast<domProfile_common>(effect->add(COLLADA_ELEMENT_PROFILE_COMMON));
            {
                // <technique sid="">
                domProfile_common::domTechniqueRef technique = daeSafeCast<domProfile_common::domTechnique>(profile->add(COLLADA_ELEMENT_TECHNIQUE));
                {
                    // <phong>
                    domProfile_common::domTechnique::domPhongRef phong = daeSafeCast<domProfile_common::domTechnique::domPhong>(technique->add(COLLADA_ELEMENT_PHONG));
                    {
                        // <ambient>
                        domFx_common_color_or_textureRef ambient = daeSafeCast<domFx_common_color_or_texture>(phong->add(COLLADA_ELEMENT_AMBIENT));
                        {
                            // <color>r g b a
                            domFx_common_color_or_texture::domColorRef ambient_color = daeSafeCast<domFx_common_color_or_texture::domColor>(ambient->add(COLLADA_ELEMENT_COLOR));
                            ambient_color->getValue().setCount(4);
                            ambient_color->getValue()[0] = color_ambient.r;
                            ambient_color->getValue()[1] = color_ambient.g;
                            ambient_color->getValue()[2] = color_ambient.b;
                            ambient_color->getValue()[3] = color_ambient.a;
                            // </color>
                        }
                        // </ambient>

                        // <diffuse>
                        domFx_common_color_or_textureRef diffuse = daeSafeCast<domFx_common_color_or_texture>(phong->add(COLLADA_ELEMENT_DIFFUSE));
                        {
                            // <color>r g b a
                            domFx_common_color_or_texture::domColorRef diffuse_color = daeSafeCast<domFx_common_color_or_texture::domColor>(diffuse->add(COLLADA_ELEMENT_COLOR));
                            diffuse_color->getValue().setCount(4);
                            diffuse_color->getValue()[0] = color_diffuse.r;
                            diffuse_color->getValue()[1] = color_diffuse.g;
                            diffuse_color->getValue()[2] = color_diffuse.b;
                            diffuse_color->getValue()[3] = color_diffuse.a;
                            // </color>
                        }
                        // </diffuse>
                    }
                    // </phong>
                }
                // </technique>
            }
            // </profile_COMMON>
        }
        // </effect>

        return effect;
    }

    /// \brief Write transformation
    /// \param pelt Element to transform
    /// \param t Transform to write
    void _WriteTransformation(daeElementRef pelt, const urdf::Pose& t)
    {
        domRotateRef prot = daeSafeCast<domRotate>(pelt->add(COLLADA_ELEMENT_ROTATE,0));
        domTranslateRef ptrans = daeSafeCast<domTranslate>(pelt->add(COLLADA_ELEMENT_TRANSLATE,0));
        ptrans->getValue().setCount(3);
        ptrans->getValue()[0] = t.position.x;
        ptrans->getValue()[1] = t.position.y;
        ptrans->getValue()[2] = t.position.z;

        prot->getValue().setCount(4);
        // extract axis from quaternion
        double sinang = t.rotation.x*t.rotation.x+t.rotation.y*t.rotation.y+t.rotation.z*t.rotation.z;
        if( std::fabs(sinang) < 1e-10 ) {
            prot->getValue()[0] = 1;
            prot->getValue()[1] = 0;
            prot->getValue()[2] = 0;
            prot->getValue()[3] = 0;
        }
        else {
            urdf::Rotation quat;
            if( t.rotation.w < 0 ) {
                quat.x = -t.rotation.x;
                quat.y = -t.rotation.y;
                quat.z = -t.rotation.z;
                quat.w = -t.rotation.w;
            }
            else {
                quat = t.rotation;
            }
            sinang = std::sqrt(sinang);
            prot->getValue()[0] = quat.x/sinang;
            prot->getValue()[1] = quat.y/sinang;
            prot->getValue()[2] = quat.z/sinang;
            prot->getValue()[3] = angles::to_degrees(2.0*std::atan2(sinang,quat.w));
        }
    }

    // binding in instance_kinematics_scene
    void _WriteBindingsInstance_kinematics_scene()
    {
        FOREACHC(it, _iasout->vkinematicsbindings) {
            domBind_kinematics_modelRef pmodelbind = daeSafeCast<domBind_kinematics_model>(_scene.kiscene->add(COLLADA_ELEMENT_BIND_KINEMATICS_MODEL));
            pmodelbind->setNode(it->second.c_str());
            daeSafeCast<domCommon_param>(pmodelbind->add(COLLADA_ELEMENT_PARAM))->setValue(it->first.c_str());
        }
        FOREACHC(it, _iasout->vaxissids) {
            domBind_joint_axisRef pjointbind = daeSafeCast<domBind_joint_axis>(_scene.kiscene->add(COLLADA_ELEMENT_BIND_JOINT_AXIS));
            pjointbind->setTarget(it->jointnodesid.c_str());
            daeSafeCast<domCommon_param>(pjointbind->add(COLLADA_ELEMENT_AXIS)->add(COLLADA_TYPE_PARAM))->setValue(it->axissid.c_str());
            daeSafeCast<domCommon_param>(pjointbind->add(COLLADA_ELEMENT_VALUE)->add(COLLADA_TYPE_PARAM))->setValue(it->valuesid.c_str());
        }
    }

private:
    static urdf::Vector3 _poseMult(const urdf::Pose& p, const urdf::Vector3& v)
    {
        double ww = 2 * p.rotation.x * p.rotation.x;
        double wx = 2 * p.rotation.x * p.rotation.y;
        double wy = 2 * p.rotation.x * p.rotation.z;
        double wz = 2 * p.rotation.x * p.rotation.w;
        double xx = 2 * p.rotation.y * p.rotation.y;
        double xy = 2 * p.rotation.y * p.rotation.z;
        double xz = 2 * p.rotation.y * p.rotation.w;
        double yy = 2 * p.rotation.z * p.rotation.z;
        double yz = 2 * p.rotation.z * p.rotation.w;
        urdf::Vector3 vnew;
        vnew.x = (1-xx-yy) * v.x + (wx-yz) * v.y + (wy+xz)*v.z + p.position.x;
        vnew.y = (wx+yz) * v.x + (1-ww-yy) * v.y + (xy-wz)*v.z + p.position.y;
        vnew.z = (wy-xz) * v.x + (xy+wz) * v.y + (1-ww-xx)*v.z + p.position.z;
        return vnew;
    }

    static urdf::Pose _poseInverse(const urdf::Pose& p)
    {
        urdf::Pose pinv;
        pinv.rotation.x = -p.rotation.x;
        pinv.rotation.y = -p.rotation.y;
        pinv.rotation.z = -p.rotation.z;
        pinv.rotation.w = p.rotation.w;
        urdf::Vector3 t = _poseMult(pinv,p.position);
        pinv.position.x = -t.x;
        pinv.position.y = -t.y;
        pinv.position.z = -t.z;
        return pinv;
    }

    static urdf::Rotation _quatMult(const urdf::Rotation& quat0, const urdf::Rotation& quat1)
    {
        urdf::Rotation q;
        q.x = quat0.w*quat1.x + quat0.x*quat1.w + quat0.y*quat1.z - quat0.z*quat1.y;
        q.y = quat0.w*quat1.y + quat0.y*quat1.w + quat0.z*quat1.x - quat0.x*quat1.z;
        q.z = quat0.w*quat1.z + quat0.z*quat1.w + quat0.x*quat1.y - quat0.y*quat1.x;
        q.w = quat0.w*quat1.w - quat0.x*quat1.x - quat0.y*quat1.y - quat0.z*quat1.z;
        double fnorm = std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
        // don't touch the divides
        q.x /= fnorm;
        q.y /= fnorm;
        q.z /= fnorm;
        q.w /= fnorm;
        return q;
    }

    static urdf::Pose _poseMult(const urdf::Pose& p0, const urdf::Pose& p1)
    {
        urdf::Pose p;
        p.position = _poseMult(p0,p1.position);
        p.rotation = _quatMult(p0.rotation,p1.rotation);
        return p;
    }

    static urdf::Rotation _quatFromMatrix(const boost::array<double,12>& mat)
    {
        urdf::Rotation rot;
        double tr = mat[4*0+0] + mat[4*1+1] + mat[4*2+2];
        if (tr >= 0) {
            rot.w = tr + 1;
            rot.x = (mat[4*2+1] - mat[4*1+2]);
            rot.y = (mat[4*0+2] - mat[4*2+0]);
            rot.z = (mat[4*1+0] - mat[4*0+1]);
        }
        else {
            // find the largest diagonal element and jump to the appropriate case
            if (mat[4*1+1] > mat[4*0+0]) {
                if (mat[4*2+2] > mat[4*1+1]) {
                    rot.z = (mat[4*2+2] - (mat[4*0+0] + mat[4*1+1])) + 1;
                    rot.x = (mat[4*2+0] + mat[4*0+2]);
                    rot.y = (mat[4*1+2] + mat[4*2+1]);
                    rot.w = (mat[4*1+0] - mat[4*0+1]);
                }
                else {
                    rot.y = (mat[4*1+1] - (mat[4*2+2] + mat[4*0+0])) + 1;
                    rot.z = (mat[4*1+2] + mat[4*2+1]);
                    rot.x = (mat[4*0+1] + mat[4*1+0]);
                    rot.w = (mat[4*0+2] - mat[4*2+0]);
                }
            }
            else if (mat[4*2+2] > mat[4*0+0]) {
                rot.z = (mat[4*2+2] - (mat[4*0+0] + mat[4*1+1])) + 1;
                rot.x = (mat[4*2+0] + mat[4*0+2]);
                rot.y = (mat[4*1+2] + mat[4*2+1]);
                rot.w = (mat[4*1+0] - mat[4*0+1]);
            }
            else {
                rot.x = (mat[4*0+0] - (mat[4*1+1] + mat[4*2+2])) + 1;
                rot.y = (mat[4*0+1] + mat[4*1+0]);
                rot.z = (mat[4*2+0] + mat[4*0+2]);
                rot.w = (mat[4*2+1] - mat[4*1+2]);
            }
        }
        double fnorm = std::sqrt(rot.x*rot.x+rot.y*rot.y+rot.z*rot.z+rot.w*rot.w);
        // don't touch the divides
        rot.x /= fnorm;
        rot.y /= fnorm;
        rot.z /= fnorm;
        rot.w /= fnorm;
        return rot;
    }

    static std::string _ComputeKinematics_modelId(int id)
    {
        return _ComputeId(str(boost::format("kmodel%d")%id));
    }

    /// \brief computes a collada-compliant sid from the urdf name
    static std::string _ComputeId(const std::string& name)
    {
        std::string newname = name;
        for(size_t i = 0; i < newname.size(); ++i) {
            if( newname[i] == '/' || newname[i] == ' ' || newname[i] == '.' ) {
                newname[i] = '_';
            }
        }
        return newname;
    }

    int _writeoptions;

    const urdf::Model& _robot;
    boost::shared_ptr<DAE> _collada;
    domCOLLADA* _dom;
    domCOLLADA::domSceneRef _globalscene;

    domLibrary_visual_scenesRef _visualScenesLib;
    domLibrary_kinematics_scenesRef _kinematicsScenesLib;
    domLibrary_kinematics_modelsRef _kinematicsModelsLib;
    domLibrary_articulated_systemsRef _articulatedSystemsLib;
    domLibrary_physics_scenesRef _physicsScenesLib;
    domLibrary_physics_modelsRef _physicsModelsLib;
    domLibrary_materialsRef _materialsLib;
    domLibrary_effectsRef _effectsLib;
    domLibrary_geometriesRef _geometriesLib;
    domTechniqueRef _sensorsLib; ///< custom sensors library
    SCENE _scene;

    boost::shared_ptr<instance_kinematics_model_output> _ikmout;
    boost::shared_ptr<instance_articulated_system_output> _iasout;
    std::map< boost::shared_ptr<const urdf::Joint>, int > _mapjointindices;
    std::map< boost::shared_ptr<const urdf::Link>, int > _maplinkindices;
    std::map< boost::shared_ptr<const urdf::Material>, int > _mapmaterialindices;
    Assimp::Importer _importer;
};


ColladaUrdfException::ColladaUrdfException(std::string const& what)
    : std::runtime_error(what)
{
}

bool colladaFromUrdfFile(string const& file, boost::shared_ptr<DAE>& dom) {
    TiXmlDocument urdf_xml;
    if (!urdf_xml.LoadFile(file)) {
        ROS_ERROR("Could not load XML file");
        return false;
    }

    return colladaFromUrdfXml(&urdf_xml, dom);
}

bool colladaFromUrdfString(string const& xml, boost::shared_ptr<DAE>& dom) {
    TiXmlDocument urdf_xml;
    if (urdf_xml.Parse(xml.c_str()) == 0) {
        ROS_ERROR("Could not parse XML document");
        return false;
    }

    return colladaFromUrdfXml(&urdf_xml, dom);
}

bool colladaFromUrdfXml(TiXmlDocument* xml_doc, boost::shared_ptr<DAE>& dom) {
    urdf::Model robot_model;
    if (!robot_model.initXml(xml_doc)) {
        ROS_ERROR("Could not generate robot model");
        return false;
    }

    return colladaFromUrdfModel(robot_model, dom);
}

bool colladaFromUrdfModel(urdf::Model const& robot_model, boost::shared_ptr<DAE>& dom) {
    ColladaWriter writer(robot_model,0);
    dom = writer.convert();
    return dom != boost::shared_ptr<DAE>();
}

bool colladaToFile(boost::shared_ptr<DAE> dom, string const& file) {
    daeString uri = dom->getDoc(0)->getDocumentURI()->getURI();
    return dom->writeTo(uri, file);
}

}
