/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2010, University of Tokyo
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

/* Author: Rosen Diankov, used OpenRAVE files for reference  */
#include <vector>
#include <list>
#include <map>
#include <stdint.h>
#include <cstdlib>
#include <cmath>
#include <string>
#include <sstream>

#include <dae.h>
#include <dae/daeErrorHandler.h>
#include <dom/domCOLLADA.h>
#include <dae/domAny.h>
#include <dom/domConstants.h>
#include <dom/domTriangles.h>
#include <dae/daeStandardURIResolver.h>

#include <boost/assert.hpp>
#include <boost/format.hpp>
#include <boost/shared_ptr.hpp>

#include <ros/ros.h>
#include <collada_parser/collada_parser.h>
#include <urdf_model/model.h>

#ifndef HAVE_MKSTEMPS
#include <fstream>
#include <fcntl.h>
#endif
#ifndef HAVE_MKSTEMPS
#include <fstream>
#include <fcntl.h>
#endif

#define FOREACH(it, v) for(typeof((v).begin())it = (v).begin(); it != (v).end(); (it)++)
#define FOREACHC FOREACH

namespace ColladaDOM150 { }

namespace urdf {

using namespace ColladaDOM150;

class UnlinkFilename
{
public:
    UnlinkFilename(const std::string& filename) : _filename(filename) {
    }
    virtual ~UnlinkFilename() {
        unlink(_filename.c_str());
    }
    std::string _filename;
};
static std::list<boost::shared_ptr<UnlinkFilename> > _listTempFilenames;

class ColladaModelReader : public daeErrorHandler
{

    class JointAxisBinding
    {
public:
        JointAxisBinding(daeElementRef pvisualtrans, domAxis_constraintRef pkinematicaxis, domCommon_float_or_paramRef jointvalue, domKinematics_axis_infoRef kinematics_axis_info, domMotion_axis_infoRef motion_axis_info) : pvisualtrans(pvisualtrans), pkinematicaxis(pkinematicaxis), jointvalue(jointvalue), kinematics_axis_info(kinematics_axis_info), motion_axis_info(motion_axis_info) {
            BOOST_ASSERT( !!pkinematicaxis );
            daeElement* pae = pvisualtrans->getParentElement();
            while (!!pae) {
                visualnode = daeSafeCast<domNode> (pae);
                if (!!visualnode) {
                    break;
                }
                pae = pae->getParentElement();
            }

            if (!visualnode) {
                ROS_WARN_STREAM(str(boost::format("couldn't find parent node of element id %s, sid %s\n")%pkinematicaxis->getID()%pkinematicaxis->getSid()));
            }
        }

        daeElementRef pvisualtrans;
        domAxis_constraintRef pkinematicaxis;
        domCommon_float_or_paramRef jointvalue;
        domNodeRef visualnode;
        domKinematics_axis_infoRef kinematics_axis_info;
        domMotion_axis_infoRef motion_axis_info;
    };

    /// \brief bindings for links between different spaces
    class LinkBinding
    {
public:
        domNodeRef node;
        domLinkRef domlink;
        domInstance_rigid_bodyRef irigidbody;
        domRigid_bodyRef rigidbody;
        domNodeRef nodephysicsoffset;
    };

    /// \brief inter-collada bindings for a kinematics scene
    class KinematicsSceneBindings
    {
public:
        std::list< std::pair<domNodeRef,domInstance_kinematics_modelRef> > listKinematicsVisualBindings;
        std::list<JointAxisBinding> listAxisBindings;
        std::list<LinkBinding> listLinkBindings;

        bool AddAxisInfo(const domInstance_kinematics_model_Array& arr, domKinematics_axis_infoRef kinematics_axis_info, domMotion_axis_infoRef motion_axis_info)
        {
            if( !kinematics_axis_info ) {
                return false;
            }
            for(size_t ik = 0; ik < arr.getCount(); ++ik) {
                daeElement* pelt = daeSidRef(kinematics_axis_info->getAxis(), arr[ik]->getUrl().getElement()).resolve().elt;
                if( !!pelt ) {
                    // look for the correct placement
                    bool bfound = false;
                    FOREACH(itbinding,listAxisBindings) {
                        if( itbinding->pkinematicaxis.cast() == pelt ) {
                            itbinding->kinematics_axis_info = kinematics_axis_info;
                            if( !!motion_axis_info ) {
                                itbinding->motion_axis_info = motion_axis_info;
                            }
                            bfound = true;
                            break;
                        }
                    }
                    if( !bfound ) {
                        ROS_WARN_STREAM(str(boost::format("could not find binding for axis: %s, %s\n")%kinematics_axis_info->getAxis()%pelt->getAttribute("sid")));
                        return false;
                    }
                    return true;
                }
            }
            ROS_WARN_STREAM(str(boost::format("could not find kinematics axis target: %s\n")%kinematics_axis_info->getAxis()));
            return false;
        }
    };

    struct USERDATA
    {
        USERDATA() {
        }
        USERDATA(double scale) : scale(scale) {
        }
        double scale;
#if URDFDOM_HEADERS_MAJOR_VERSION < 1
        boost::shared_ptr<void> p; ///< custom managed data
#else
        std::shared_ptr<void> p; ///< custom managed data
#endif
    };

    enum GeomType {
        GeomNone = 0,
        GeomBox = 1,
        GeomSphere = 2,
        GeomCylinder = 3,
        GeomTrimesh = 4,
    };

    struct GEOMPROPERTIES
    {
        Pose _t;                ///< local transformation of the geom primitive with respect to the link's coordinate system
        Vector3 vGeomData; ///< for boxes, first 3 values are extents
        ///< for sphere it is radius
        ///< for cylinder, first 2 values are radius and height
        ///< for trimesh, none
        Color diffuseColor, ambientColor; ///< hints for how to color the meshes
        std::vector<Vector3> vertices;
        std::vector<int> indices;

        ///< discretization value is chosen. Should be transformed by _t before rendering
        GeomType type;         ///< the type of geometry primitive


        // generate a sphere triangulation starting with an icosahedron
        // all triangles are oriented counter clockwise
        static void GenerateSphereTriangulation(std::vector<Vector3> realvertices, std::vector<int> realindices, int levels)
        {
            const double GTS_M_ICOSAHEDRON_X = 0.850650808352039932181540497063011072240401406;
            const double GTS_M_ICOSAHEDRON_Y = 0.525731112119133606025669084847876607285497935;
            const double GTS_M_ICOSAHEDRON_Z  = 0;
            std::vector<Vector3> tempvertices[2];
            std::vector<int> tempindices[2];

            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_Z, +GTS_M_ICOSAHEDRON_X, -GTS_M_ICOSAHEDRON_Y));
            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_X, +GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z));
            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z, -GTS_M_ICOSAHEDRON_X));
            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z, +GTS_M_ICOSAHEDRON_X));
            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_X, -GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z));
            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_Z, +GTS_M_ICOSAHEDRON_X, +GTS_M_ICOSAHEDRON_Y));
            tempvertices[0].push_back(Vector3(-GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z, +GTS_M_ICOSAHEDRON_X));
            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_Z, -GTS_M_ICOSAHEDRON_X, -GTS_M_ICOSAHEDRON_Y));
            tempvertices[0].push_back(Vector3(-GTS_M_ICOSAHEDRON_X, +GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z));
            tempvertices[0].push_back(Vector3(-GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z, -GTS_M_ICOSAHEDRON_X));
            tempvertices[0].push_back(Vector3(-GTS_M_ICOSAHEDRON_X, -GTS_M_ICOSAHEDRON_Y, +GTS_M_ICOSAHEDRON_Z));
            tempvertices[0].push_back(Vector3(+GTS_M_ICOSAHEDRON_Z, -GTS_M_ICOSAHEDRON_X, +GTS_M_ICOSAHEDRON_Y));

            const int nindices=60;
            int indices[nindices] = {
                0, 1, 2,
                1, 3, 4,
                3, 5, 6,
                2, 4, 7,
                5, 6, 8,
                2, 7, 9,
                0, 5, 8,
                7, 9, 10,
                0, 1, 5,
                7, 10, 11,
                1, 3, 5,
                6, 10, 11,
                3, 6, 11,
                9, 10, 8,
                3, 4, 11,
                6, 8, 10,
                4, 7, 11,
                1, 2, 4,
                0, 8, 9,
                0, 2, 9
            };

            Vector3 v[3];
            // make sure oriented CCW
            for(int i = 0; i < nindices; i += 3 ) {
                v[0] = tempvertices[0][indices[i]];
                v[1] = tempvertices[0][indices[i+1]];
                v[2] = tempvertices[0][indices[i+2]];
                if( _dot3(v[0], _cross3(_sub3(v[1],v[0]),_sub3(v[2],v[0]))) < 0 ) {
                    std::swap(indices[i], indices[i+1]);
                }
            }

            tempindices[0].resize(nindices);
            std::copy(&indices[0],&indices[nindices],tempindices[0].begin());
            std::vector<Vector3>* curvertices = &tempvertices[0], *newvertices = &tempvertices[1];
            std::vector<int> *curindices = &tempindices[0], *newindices = &tempindices[1];
            while(levels-- > 0) {

                newvertices->resize(0);
                newvertices->reserve(2*curvertices->size());
                newvertices->insert(newvertices->end(), curvertices->begin(), curvertices->end());
                newindices->resize(0);
                newindices->reserve(4*curindices->size());

                std::map< uint64_t, int > mapnewinds;
                std::map< uint64_t, int >::iterator it;

                for(size_t i = 0; i < curindices->size(); i += 3) {
                    // for ever tri, create 3 new vertices and 4 new triangles.
                    v[0] = curvertices->at(curindices->at(i));
                    v[1] = curvertices->at(curindices->at(i+1));
                    v[2] = curvertices->at(curindices->at(i+2));

                    int inds[3];
                    for(int j = 0; j < 3; ++j) {
                        uint64_t key = ((uint64_t)curindices->at(i+j)<<32)|(uint64_t)curindices->at(i + ((j+1)%3));
                        it = mapnewinds.find(key);

                        if( it == mapnewinds.end() ) {
                            inds[j] = mapnewinds[key] = mapnewinds[(key<<32)|(key>>32)] = (int)newvertices->size();
                            newvertices->push_back(_normalize3(_add3(v[j],v[(j+1)%3 ])));
                        }
                        else {
                            inds[j] = it->second;
                        }
                    }

                    newindices->push_back(curindices->at(i));    newindices->push_back(inds[0]);    newindices->push_back(inds[2]);
                    newindices->push_back(inds[0]);    newindices->push_back(curindices->at(i+1));    newindices->push_back(inds[1]);
                    newindices->push_back(inds[2]);    newindices->push_back(inds[0]);    newindices->push_back(inds[1]);
                    newindices->push_back(inds[2]);    newindices->push_back(inds[1]);    newindices->push_back(curindices->at(i+2));
                }

                std::swap(newvertices,curvertices);
                std::swap(newindices,curindices);
            }

            realvertices = *curvertices;
            realindices = *curindices;
        }

        bool InitCollisionMesh(double fTessellation=1.0)
        {
            if( type == GeomTrimesh ) {
                return true;
            }
            indices.clear();
            vertices.clear();

            if( fTessellation < 0.01f ) {
                fTessellation = 0.01f;
            }
            // start tesselating
            switch(type) {
            case GeomSphere: {
                // log_2 (1+ tess)
                GenerateSphereTriangulation(vertices,indices, 3 + (int)(logf(fTessellation) / logf(2.0f)) );
                double fRadius = vGeomData.x;
                FOREACH(it, vertices) {
                    it->x *= fRadius;
                    it->y *= fRadius;
                    it->z *= fRadius;
                }
                break;
            }
            case GeomBox: {
                // trivial
                Vector3 ex = vGeomData;
                Vector3 v[8] = { Vector3(ex.x, ex.y, ex.z),
                                 Vector3(ex.x, ex.y, -ex.z),
                                 Vector3(ex.x, -ex.y, ex.z),
                                 Vector3(ex.x, -ex.y, -ex.z),
                                 Vector3(-ex.x, ex.y, ex.z),
                                 Vector3(-ex.x, ex.y, -ex.z),
                                 Vector3(-ex.x, -ex.y, ex.z),
                                 Vector3(-ex.x, -ex.y, -ex.z) };
                const int nindices = 36;
                int startindices[] = {
                    0, 1, 2,
                    1, 2, 3,
                    4, 5, 6,
                    5, 6, 7,
                    0, 1, 4,
                    1, 4, 5,
                    2, 3, 6,
                    3, 6, 7,
                    0, 2, 4,
                    2, 4, 6,
                    1, 3, 5,
                    3, 5, 7
                };

                for(int i = 0; i < nindices; i += 3 ) {
                    Vector3 v1 = v[startindices[i]];
                    Vector3 v2 = v[startindices[i+1]];
                    Vector3 v3 = v[startindices[i+2]];
                    if( _dot3(v1, _sub3(v2,_cross3(v1, _sub3(v3,v1)))) < 0 ) {
                        std::swap(indices[i], indices[i+1]);
                    }
                }

                vertices.resize(8);
                std::copy(&v[0],&v[8],vertices.begin());
                indices.resize(nindices);
                std::copy(&startindices[0],&startindices[nindices],indices.begin());
                break;
            }
            case GeomCylinder: {
                // cylinder is on y axis
                double rad = vGeomData.x, len = vGeomData.y*0.5f;

                int numverts = (int)(fTessellation*24.0f) + 3;
                double dtheta = 2 * M_PI / (double)numverts;
                vertices.push_back(Vector3(0,0,len));
                vertices.push_back(Vector3(0,0,-len));
                vertices.push_back(Vector3(rad,0,len));
                vertices.push_back(Vector3(rad,0,-len));

                for(int i = 0; i < numverts+1; ++i) {
                    double s = rad * std::sin(dtheta * (double)i);
                    double c = rad * std::cos(dtheta * (double)i);

                    int off = (int)vertices.size();
                    vertices.push_back(Vector3(c, s, len));
                    vertices.push_back(Vector3(c, s, -len));

                    indices.push_back(0);       indices.push_back(off);       indices.push_back(off-2);
                    indices.push_back(1);       indices.push_back(off-1);       indices.push_back(off+1);
                    indices.push_back(off-2);   indices.push_back(off);         indices.push_back(off-1);
                    indices.push_back(off);   indices.push_back(off-1);         indices.push_back(off+1);
                }
                break;
            }
            default:
                BOOST_ASSERT(0);
            }
            return true;
        }
    };

public:
    ColladaModelReader(urdf::ModelInterfaceSharedPtr model) : _dom(NULL), _nGlobalSensorId(0), _nGlobalManipulatorId(0), _model(model) {
        daeErrorHandler::setErrorHandler(this);
        _resourcedir = ".";
    }
    virtual ~ColladaModelReader() {
        _vuserdata.clear();
        _collada.reset();
        DAE::cleanup();
    }

    bool InitFromFile(const std::string& filename) {
        ROS_DEBUG_STREAM(str(boost::format("init COLLADA reader version: %s, namespace: %s, filename: %s\n")%COLLADA_VERSION%COLLADA_NAMESPACE%filename));
        _collada.reset(new DAE);
        _dom = (domCOLLADA*)_collada->open(filename);
        if (!_dom) {
            return false;
        }
        _filename=filename;

        size_t maxchildren = _countChildren(_dom);
        _vuserdata.resize(0);
        _vuserdata.reserve(maxchildren);

        double dScale = 1.0;
        _processUserData(_dom, dScale);
        ROS_DEBUG_STREAM(str(boost::format("processed children: %d/%d\n")%_vuserdata.size()%maxchildren));
        return _Extract();
    }

    bool InitFromData(const std::string& pdata) {
        ROS_DEBUG_STREAM(str(boost::format("init COLLADA reader version: %s, namespace: %s\n")%COLLADA_VERSION%COLLADA_NAMESPACE));
        _collada.reset(new DAE);
        _dom = (domCOLLADA*)_collada->openFromMemory(".",pdata.c_str());
        if (!_dom) {
            return false;
        }

        size_t maxchildren = _countChildren(_dom);
        _vuserdata.resize(0);
        _vuserdata.reserve(maxchildren);

        double dScale = 1.0;
        _processUserData(_dom, dScale);
        ROS_DEBUG_STREAM(str(boost::format("processed children: %d/%d\n")%_vuserdata.size()%maxchildren));
        return _Extract();
    }

protected:

    /// \extract the first possible robot in the scene
    bool _Extract()
    {
        _model->clear();
        std::list< std::pair<domInstance_kinematics_modelRef, boost::shared_ptr<KinematicsSceneBindings> > > listPossibleBodies;
        domCOLLADA::domSceneRef allscene = _dom->getScene();
        if( !allscene ) {
            return false;
        }

        //  parse each instance kinematics scene, prioritize real robots
        for (size_t iscene = 0; iscene < allscene->getInstance_kinematics_scene_array().getCount(); iscene++) {
            domInstance_kinematics_sceneRef kiscene = allscene->getInstance_kinematics_scene_array()[iscene];
            domKinematics_sceneRef kscene = daeSafeCast<domKinematics_scene> (kiscene->getUrl().getElement().cast());
            if (!kscene) {
                continue;
            }
            boost::shared_ptr<KinematicsSceneBindings> bindings(new KinematicsSceneBindings());
            _ExtractKinematicsVisualBindings(allscene->getInstance_visual_scene(),kiscene,*bindings);
            _ExtractPhysicsBindings(allscene,*bindings);
            for(size_t ias = 0; ias < kscene->getInstance_articulated_system_array().getCount(); ++ias) {
                if( _ExtractArticulatedSystem(kscene->getInstance_articulated_system_array()[ias], *bindings) ) {
                    _PostProcess();
                    return true;
                }
            }
            for(size_t ikmodel = 0; ikmodel < kscene->getInstance_kinematics_model_array().getCount(); ++ikmodel) {
                listPossibleBodies.push_back(std::make_pair(kscene->getInstance_kinematics_model_array()[ikmodel], bindings));
            }
        }

        FOREACH(it, listPossibleBodies) {
            if( _ExtractKinematicsModel(it->first, *it->second) ) {
                _PostProcess();
                return true;
            }
        }

        return false;
    }

    void _PostProcess()
    {
        std::map<std::string, std::string> parent_link_tree;
        // building tree: name mapping
        try
        {
            _model->initTree(parent_link_tree);
        }
        catch(ParseError &e)
        {
            ROS_ERROR("Failed to build tree: %s", e.what());
        }

        // find the root link
        try
        {
            _model->initRoot(parent_link_tree);
        }
        catch(ParseError &e)
        {
            ROS_ERROR("Failed to find root link: %s", e.what());
        }
    }

    /// \brief extracts an articulated system. Note that an articulated system can include other articulated systems
    bool _ExtractArticulatedSystem(domInstance_articulated_systemRef ias, KinematicsSceneBindings& bindings)
    {
        if( !ias ) {
            return false;
        }
        ROS_DEBUG_STREAM(str(boost::format("instance articulated system sid %s\n")%ias->getSid()));
        domArticulated_systemRef articulated_system = daeSafeCast<domArticulated_system> (ias->getUrl().getElement().cast());
        if( !articulated_system ) {
            return false;
        }

        boost::shared_ptr<std::string> pinterface_type = _ExtractInterfaceType(ias->getExtra_array());
        if( !pinterface_type ) {
            pinterface_type = _ExtractInterfaceType(articulated_system->getExtra_array());
        }
        if( !!pinterface_type ) {
            ROS_DEBUG_STREAM(str(boost::format("robot type: %s")%(*pinterface_type)));
        }

        // set the name
        if( _model->name_.size() == 0 && !!ias->getName() ) {
            _model->name_ = ias->getName();
        }
        if( _model->name_.size() == 0 && !!ias->getSid()) {
            _model->name_ = ias->getSid();
        }
        if( _model->name_.size() == 0 && !!articulated_system->getName() ) {
            _model->name_ = articulated_system->getName();
        }
        if( _model->name_.size() == 0 && !!articulated_system->getId()) {
            _model->name_ = articulated_system->getId();
        }

        if( !!articulated_system->getMotion() ) {
            domInstance_articulated_systemRef ias_new = articulated_system->getMotion()->getInstance_articulated_system();
            if( !!articulated_system->getMotion()->getTechnique_common() ) {
                for(size_t i = 0; i < articulated_system->getMotion()->getTechnique_common()->getAxis_info_array().getCount(); ++i) {
                    domMotion_axis_infoRef motion_axis_info = articulated_system->getMotion()->getTechnique_common()->getAxis_info_array()[i];
                    // this should point to a kinematics axis_info
                    domKinematics_axis_infoRef kinematics_axis_info = daeSafeCast<domKinematics_axis_info>(daeSidRef(motion_axis_info->getAxis(), ias_new->getUrl().getElement()).resolve().elt);
                    if( !!kinematics_axis_info ) {
                        // find the parent kinematics and go through all its instance kinematics models
                        daeElement* pparent = kinematics_axis_info->getParent();
                        while(!!pparent && pparent->typeID() != domKinematics::ID()) {
                            pparent = pparent->getParent();
                        }
                        BOOST_ASSERT(!!pparent);
                        bindings.AddAxisInfo(daeSafeCast<domKinematics>(pparent)->getInstance_kinematics_model_array(), kinematics_axis_info, motion_axis_info);
                    }
                    else {
                        ROS_WARN_STREAM(str(boost::format("failed to find kinematics axis %s\n")%motion_axis_info->getAxis()));
                    }
                }
            }
            if( !_ExtractArticulatedSystem(ias_new,bindings) ) {
                return false;
            }
        }
        else {
            if( !articulated_system->getKinematics() ) {
                ROS_WARN_STREAM(str(boost::format("collada <kinematics> tag empty? instance_articulated_system=%s\n")%ias->getID()));
                return true;
            }

            if( !!articulated_system->getKinematics()->getTechnique_common() ) {
                for(size_t i = 0; i < articulated_system->getKinematics()->getTechnique_common()->getAxis_info_array().getCount(); ++i) {
                    bindings.AddAxisInfo(articulated_system->getKinematics()->getInstance_kinematics_model_array(), articulated_system->getKinematics()->getTechnique_common()->getAxis_info_array()[i], NULL);
                }
            }

            for(size_t ik = 0; ik < articulated_system->getKinematics()->getInstance_kinematics_model_array().getCount(); ++ik) {
                _ExtractKinematicsModel(articulated_system->getKinematics()->getInstance_kinematics_model_array()[ik],bindings);
            }
        }

        _ExtractRobotAttachedActuators(articulated_system);
        _ExtractRobotManipulators(articulated_system);
        _ExtractRobotAttachedSensors(articulated_system);
        return true;
    }

    bool _ExtractKinematicsModel(domInstance_kinematics_modelRef ikm, KinematicsSceneBindings& bindings)
    {
        if( !ikm ) {
            return false;
        }
        ROS_DEBUG_STREAM(str(boost::format("instance kinematics model sid %s\n")%ikm->getSid()));
        domKinematics_modelRef kmodel = daeSafeCast<domKinematics_model> (ikm->getUrl().getElement().cast());
        if (!kmodel) {
            ROS_WARN_STREAM(str(boost::format("%s does not reference valid kinematics\n")%ikm->getSid()));
            return false;
        }
        domPhysics_modelRef pmodel;
        boost::shared_ptr<std::string> pinterface_type = _ExtractInterfaceType(ikm->getExtra_array());
        if( !pinterface_type ) {
            pinterface_type = _ExtractInterfaceType(kmodel->getExtra_array());
        }
        if( !!pinterface_type ) {
            ROS_DEBUG_STREAM(str(boost::format("kinbody interface type: %s")%(*pinterface_type)));
        }

        // find matching visual node
        domNodeRef pvisualnode;
        FOREACH(it, bindings.listKinematicsVisualBindings) {
            if( it->second == ikm ) {
                pvisualnode = it->first;
                break;
            }
        }
        if( !pvisualnode ) {
            ROS_WARN_STREAM(str(boost::format("failed to find visual node for instance kinematics model %s\n")%ikm->getSid()));
            return false;
        }

        if( _model->name_.size() == 0 && !!ikm->getName() ) {
            _model->name_ = ikm->getName();
        }
        if( _model->name_.size() == 0 && !!ikm->getID() ) {
            _model->name_ = ikm->getID();
        }

        if (!_ExtractKinematicsModel(kmodel, pvisualnode, pmodel, bindings)) {
            ROS_WARN_STREAM(str(boost::format("failed to load kinbody from kinematics model %s\n")%kmodel->getID()));
            return false;
        }
        return true;
    }

    /// \brief append the kinematics model to the openrave kinbody
    bool _ExtractKinematicsModel(domKinematics_modelRef kmodel, domNodeRef pnode, domPhysics_modelRef pmodel, const KinematicsSceneBindings& bindings)
    {
        std::vector<domJointRef> vdomjoints;
        ROS_DEBUG_STREAM(str(boost::format("kinematics model: %s\n")%_model->name_));
        if( !!pnode ) {
            ROS_DEBUG_STREAM(str(boost::format("node name: %s\n")%pnode->getId()));
        }

        //  Process joint of the kinbody
        domKinematics_model_techniqueRef ktec = kmodel->getTechnique_common();

        //  Store joints
        for (size_t ijoint = 0; ijoint < ktec->getJoint_array().getCount(); ++ijoint) {
            vdomjoints.push_back(ktec->getJoint_array()[ijoint]);
        }

        //  Store instances of joints
        for (size_t ijoint = 0; ijoint < ktec->getInstance_joint_array().getCount(); ++ijoint) {
            domJointRef pelt = daeSafeCast<domJoint> (ktec->getInstance_joint_array()[ijoint]->getUrl().getElement());
            if (!pelt) {
                ROS_WARN_STREAM("failed to get joint from instance\n");
            }
            else {
                vdomjoints.push_back(pelt);
            }
        }

        ROS_DEBUG_STREAM(str(boost::format("Number of root links in the kmodel %d\n")%ktec->getLink_array().getCount()));
        for (size_t ilink = 0; ilink < ktec->getLink_array().getCount(); ++ilink) {
            domLinkRef pdomlink = ktec->getLink_array()[ilink];
            _RootOrigin  = _poseFromMatrix(_ExtractFullTransform(pdomlink));
            ROS_DEBUG("RootOrigin: %s %lf %lf %lf %lf %lf %lf %lf",
                      pdomlink->getName(),
                      _RootOrigin.position.x, _RootOrigin.position.y, _RootOrigin.position.z,
                      _RootOrigin.rotation.x, _RootOrigin.rotation.y, _RootOrigin.rotation.z, _RootOrigin.rotation.w);

            domNodeRef pvisualnode;
            FOREACH(it, bindings.listKinematicsVisualBindings) {
              if(strcmp(it->first->getName() ,pdomlink->getName()) == 0) {
                pvisualnode = it->first;
                break;
              }
            }
            if (!!pvisualnode) {
              _VisualRootOrigin  = _poseFromMatrix(_getNodeParentTransform(pvisualnode));
              ROS_DEBUG("VisualRootOrigin: %s %lf %lf %lf %lf %lf %lf %lf",
                        pdomlink->getName(),
                        _VisualRootOrigin.position.x, _VisualRootOrigin.position.y, _VisualRootOrigin.position.z,
                        _VisualRootOrigin.rotation.x, _VisualRootOrigin.rotation.y, _VisualRootOrigin.rotation.z, _VisualRootOrigin.rotation.w);
            }
            _ExtractLink(pdomlink, ilink == 0 ? pnode : domNodeRef(), Pose(), Pose(), vdomjoints, bindings);
        }

        //  TODO: implement mathml
        for (size_t iform = 0; iform < ktec->getFormula_array().getCount(); ++iform) {
            domFormulaRef pf = ktec->getFormula_array()[iform];
            if (!pf->getTarget()) {
                ROS_WARN_STREAM("formula target not valid\n");
                continue;
            }

            // find the target joint
            urdf::JointSharedPtr pjoint = _getJointFromRef(pf->getTarget()->getParam()->getValue(),pf);
            if (!pjoint) {
                continue;
            }

            if (!!pf->getTechnique_common()) {
                daeElementRef peltmath;
                daeTArray<daeElementRef> children;
                pf->getTechnique_common()->getChildren(children);
                for (size_t ichild = 0; ichild < children.getCount(); ++ichild) {
                    daeElementRef pelt = children[ichild];
                    if (_checkMathML(pelt,std::string("math")) ) {
                        peltmath = pelt;
                    }
                    else {
                        ROS_WARN_STREAM(str(boost::format("unsupported formula element: %s\n")%pelt->getElementName()));
                    }
                }
                if (!!peltmath) {
                    // full math xml spec not supported, only looking for ax+b pattern:
                    // <apply> <plus/> <apply> <times/> <ci>a</ci> x </apply> <ci>b</ci> </apply>
                    double a = 1, b = 0;
                    daeElementRef psymboljoint;
                    BOOST_ASSERT(peltmath->getChildren().getCount()>0);
                    daeElementRef papplyelt = peltmath->getChildren()[0];
                    BOOST_ASSERT(_checkMathML(papplyelt,"apply"));
                    BOOST_ASSERT(papplyelt->getChildren().getCount()>0);
                    if( _checkMathML(papplyelt->getChildren()[0],"plus") ) {
                        BOOST_ASSERT(papplyelt->getChildren().getCount()==3);
                        daeElementRef pa = papplyelt->getChildren()[1];
                        daeElementRef pb = papplyelt->getChildren()[2];
                        if( !_checkMathML(papplyelt->getChildren()[1],"apply") ) {
                            std::swap(pa,pb);
                        }
                        if( !_checkMathML(pa,"csymbol") ) {
                            BOOST_ASSERT(_checkMathML(pa,"apply"));
                            BOOST_ASSERT(_checkMathML(pa->getChildren()[0],"times"));
                            if( _checkMathML(pa->getChildren()[1],"csymbol") ) {
                                psymboljoint = pa->getChildren()[1];
                                BOOST_ASSERT(_checkMathML(pa->getChildren()[2],"cn"));
                                std::stringstream ss(pa->getChildren()[2]->getCharData());
                                ss >> a;
                            }
                            else {
                                psymboljoint = pa->getChildren()[2];
                                BOOST_ASSERT(_checkMathML(pa->getChildren()[1],"cn"));
                                std::stringstream ss(pa->getChildren()[1]->getCharData());
                                ss >> a;
                            }
                        }
                        else {
                            psymboljoint = pa;
                        }
                        BOOST_ASSERT(_checkMathML(pb,"cn"));
                        {
                            std::stringstream ss(pb->getCharData());
                            ss >> b;
                        }
                    }
                    else if( _checkMathML(papplyelt->getChildren()[0],"minus") ) {
                        BOOST_ASSERT(_checkMathML(papplyelt->getChildren()[1],"csymbol"));
                        a = -1;
                        psymboljoint = papplyelt->getChildren()[1];
                    }
                    else {
                        BOOST_ASSERT(_checkMathML(papplyelt->getChildren()[0],"csymbol"));
                        psymboljoint = papplyelt->getChildren()[0];
                    }
                    BOOST_ASSERT(psymboljoint->hasAttribute("encoding"));
                    BOOST_ASSERT(psymboljoint->getAttribute("encoding")==std::string("COLLADA"));
                    urdf::JointSharedPtr pbasejoint = _getJointFromRef(psymboljoint->getCharData().c_str(),pf);
                    if( !!pbasejoint ) {
                        // set the mimic properties
                        pjoint->mimic.reset(new JointMimic());
                        pjoint->mimic->joint_name = pbasejoint->name;
                        pjoint->mimic->multiplier = a;
                        pjoint->mimic->offset = b;
                        ROS_DEBUG_STREAM(str(boost::format("assigning joint %s to mimic %s %f %f\n")%pjoint->name%pbasejoint->name%a%b));
                    }
                }
            }
        }
        return true;
    }

    ///  \brief Extract Link info and add it to an existing body
    urdf::LinkSharedPtr _ExtractLink(const domLinkRef pdomlink,const domNodeRef pdomnode, const Pose& tParentWorldLink, const Pose& tParentLink, const std::vector<domJointRef>& vdomjoints, const KinematicsSceneBindings& bindings) {
        const std::list<JointAxisBinding>& listAxisBindings = bindings.listAxisBindings;
        //  Set link name with the name of the COLLADA's Link
        std::string linkname = _ExtractLinkName(pdomlink);
        if( linkname.size() == 0 ) {
            ROS_WARN_STREAM("<link> has no name or id, falling back to <node>!\n");
            if( !!pdomnode ) {
                if (!!pdomnode->getName()) {
                    linkname = pdomnode->getName();
                }
                if( linkname.size() == 0 && !!pdomnode->getID()) {
                    linkname = pdomnode->getID();
                }
            }
        }

        urdf::LinkSharedPtr plink;
        _model->getLink(linkname,plink);
        if( !plink ) {
            plink.reset(new Link());
            plink->name = linkname;
            plink->visual.reset(new Visual());
            plink->visual->material_name = "";
            plink->visual->material.reset(new Material());
            plink->visual->material->name = "Red";
            plink->visual->material->color.r = 0.0;
            plink->visual->material->color.g = 1.0;
            plink->visual->material->color.b = 0.0;
            plink->visual->material->color.a = 1.0;
            plink->inertial.reset();
            _model->links_.insert(std::make_pair(linkname,plink));
        }

        _getUserData(pdomlink)->p = plink;
        if( !!pdomnode ) {
            ROS_DEBUG_STREAM(str(boost::format("Node Id %s and Name %s\n")%pdomnode->getId()%pdomnode->getName()));
        }

        // physics
        domInstance_rigid_bodyRef irigidbody;
        domRigid_bodyRef rigidbody;
        bool bFoundBinding = false;
        FOREACH(itlinkbinding, bindings.listLinkBindings) {
            if( !!pdomnode->getID() && !!itlinkbinding->node->getID() && strcmp(pdomnode->getID(),itlinkbinding->node->getID()) == 0 ) {
                bFoundBinding = true;
                irigidbody = itlinkbinding->irigidbody;
                rigidbody = itlinkbinding->rigidbody;
            }
        }

        if( !!rigidbody && !!rigidbody->getTechnique_common() ) {
            domRigid_body::domTechnique_commonRef rigiddata = rigidbody->getTechnique_common();
            if( !!rigiddata->getMass() ) {
                if ( !plink->inertial ) {
                    plink->inertial.reset(new Inertial());
                }
                plink->inertial->mass = rigiddata->getMass()->getValue();
            }
            if( !!rigiddata->getInertia() ) {
                if ( !plink->inertial ) {
                    plink->inertial.reset(new Inertial());
                }
                plink->inertial->ixx = rigiddata->getInertia()->getValue()[0];
                plink->inertial->iyy = rigiddata->getInertia()->getValue()[1];
                plink->inertial->izz = rigiddata->getInertia()->getValue()[2];
            }
            if( !!rigiddata->getMass_frame() ) {
                if ( !plink->inertial ) {
                    plink->inertial.reset(new Inertial());
                }
                //plink->inertial->origin = _poseMult(_poseInverse(tParentWorldLink), _poseFromMatrix(_ExtractFullTransform(rigiddata->getMass_frame())));
                Pose tlink = _poseFromMatrix(_ExtractFullTransform(pdomlink));
                plink->inertial->origin = _poseMult(_poseInverse(_poseMult(_poseInverse(_RootOrigin),
                                                                           _poseMult(tParentWorldLink, tlink))),
                                                    _poseFromMatrix(_ExtractFullTransform(rigiddata->getMass_frame())));
            }
        }

        std::list<GEOMPROPERTIES> listGeomProperties;
        if (!pdomlink) {
            ROS_WARN_STREAM("Extract object NOT kinematics !!!\n");
            _ExtractGeometry(pdomnode,listGeomProperties,listAxisBindings,Pose());
        }
        else {
            ROS_DEBUG_STREAM(str(boost::format("Attachment link elements: %d\n")%pdomlink->getAttachment_full_array().getCount()));
            Pose tlink = _poseFromMatrix(_ExtractFullTransform(pdomlink));
            ROS_DEBUG("tlink: %s: %lf %lf %lf %lf %lf %lf %lf",
                      linkname.c_str(),
                      tlink.position.x,  tlink.position.y, tlink.position.z,
                      tlink.rotation.x, tlink.rotation.y, tlink.rotation.z, tlink.rotation.w);
            plink->visual->origin = _poseMult(tParentLink, tlink); // use the kinematics coordinate system for each link
            //            ROS_INFO("link %s rot: %f %f %f %f",linkname.c_str(),plink->visual->origin.rotation.w, plink->visual->origin.rotation.x,plink->visual->origin.rotation.y,plink->visual->origin.rotation.z);
            //            ROS_INFO("link %s trans: %f %f %f",linkname.c_str(),plink->visual->origin.position.x,plink->visual->origin.position.y,plink->visual->origin.position.z);

            // Get the geometry
            _ExtractGeometry(pdomnode, listGeomProperties, listAxisBindings,
                             _poseMult(_poseMult(tParentWorldLink,tlink), plink->visual->origin));

            ROS_DEBUG_STREAM(str(boost::format("After ExtractGeometry Attachment link elements: %d\n")%pdomlink->getAttachment_full_array().getCount()));

            //  Process all atached links
            for (size_t iatt = 0; iatt < pdomlink->getAttachment_full_array().getCount(); ++iatt) {
                domLink::domAttachment_fullRef pattfull = pdomlink->getAttachment_full_array()[iatt];

                // get link kinematics transformation
                Pose tatt = _poseFromMatrix(_ExtractFullTransform(pattfull));

                // process attached links
                daeElement* peltjoint = daeSidRef(pattfull->getJoint(), pattfull).resolve().elt;
                domJointRef pdomjoint = daeSafeCast<domJoint> (peltjoint);

                if (!pdomjoint) {
                    domInstance_jointRef pdomijoint = daeSafeCast<domInstance_joint> (peltjoint);
                    if (!!pdomijoint) {
                        pdomjoint = daeSafeCast<domJoint> (pdomijoint->getUrl().getElement().cast());
                    }
                }

                if (!pdomjoint || pdomjoint->typeID() != domJoint::ID()) {
                    ROS_WARN_STREAM(str(boost::format("could not find attached joint %s!\n")%pattfull->getJoint()));
                    return urdf::LinkSharedPtr();
                }

                // get direct child link
                if (!pattfull->getLink()) {
                    ROS_WARN_STREAM(str(boost::format("joint %s needs to be attached to a valid link\n")%pdomjoint->getID()));
                    continue;
                }

                // find the correct joint in the bindings
                daeTArray<domAxis_constraintRef> vdomaxes = pdomjoint->getChildrenByType<domAxis_constraint>();
                domNodeRef pchildnode;

                // see if joint has a binding to a visual node
                FOREACHC(itaxisbinding,listAxisBindings) {
                    for (size_t ic = 0; ic < vdomaxes.getCount(); ++ic) {
                        //  If the binding for the joint axis is found, retrieve the info
                        if (vdomaxes[ic] == itaxisbinding->pkinematicaxis) {
                            pchildnode = itaxisbinding->visualnode;
                            break;
                        }
                    }
                    if( !!pchildnode ) {
                        break;
                    }
                }
                if (!pchildnode) {
                    ROS_DEBUG_STREAM(str(boost::format("joint %s has no visual binding\n")%pdomjoint->getID()));
                }

                // create the joints before creating the child links
                std::vector<urdf::JointSharedPtr > vjoints(vdomaxes.getCount());
                for (size_t ic = 0; ic < vdomaxes.getCount(); ++ic) {
                    bool joint_active = true; // if not active, put into the passive list
                    FOREACHC(itaxisbinding,listAxisBindings) {
                        if (vdomaxes[ic] == itaxisbinding->pkinematicaxis) {
                            if( !!itaxisbinding->kinematics_axis_info ) {
                                if( !!itaxisbinding->kinematics_axis_info->getActive() ) {
                                    joint_active = resolveBool(itaxisbinding->kinematics_axis_info->getActive(),itaxisbinding->kinematics_axis_info);
                                }
                            }
                            break;
                        }
                    }

                    urdf::JointSharedPtr pjoint(new Joint());
                    pjoint->limits.reset(new JointLimits());
                    pjoint->limits->velocity = 0.0;
                    pjoint->limits->effort = 0.0;
                    pjoint->parent_link_name = plink->name;

                    if( !!pdomjoint->getName() ) {
                        pjoint->name = pdomjoint->getName();
                    }
                    else {
                        pjoint->name = str(boost::format("dummy%d")%_model->joints_.size());
                    }

                    if( !joint_active ) {
                        ROS_INFO_STREAM(str(boost::format("joint %s is passive, but adding to hierarchy\n")%pjoint->name));
                    }

                    domAxis_constraintRef pdomaxis = vdomaxes[ic];
                    if( strcmp(pdomaxis->getElementName(), "revolute") == 0 ) {
                        pjoint->type = Joint::REVOLUTE;
                    }
                    else if( strcmp(pdomaxis->getElementName(), "prismatic") == 0 ) {
                        pjoint->type = Joint::PRISMATIC;
                    }
                    else {
                        ROS_WARN_STREAM(str(boost::format("unsupported joint type: %s\n")%pdomaxis->getElementName()));
                    }

                    _getUserData(pdomjoint)->p = pjoint;
#if URDFDOM_HEADERS_MAJOR_VERSION < 1
                    _getUserData(pdomaxis)->p = boost::shared_ptr<int>(new int(_model->joints_.size()));
#else
                    _getUserData(pdomaxis)->p = std::shared_ptr<int>(new int(_model->joints_.size()));
#endif
                    _model->joints_[pjoint->name] = pjoint;
                    vjoints[ic] = pjoint;
                }

                urdf::LinkSharedPtr pchildlink = _ExtractLink(pattfull->getLink(), pchildnode, _poseMult(_poseMult(tParentWorldLink,tlink), tatt), tatt, vdomjoints, bindings);

                if (!pchildlink) {
                    ROS_WARN_STREAM(str(boost::format("Link has no child: %s\n")%plink->name));
                    continue;
                }

                int numjoints = 0;
                for (size_t ic = 0; ic < vdomaxes.getCount(); ++ic) {
                    domKinematics_axis_infoRef kinematics_axis_info;
                    domMotion_axis_infoRef motion_axis_info;
                    FOREACHC(itaxisbinding,listAxisBindings) {
                        bool bfound = false;
                        if (vdomaxes[ic] == itaxisbinding->pkinematicaxis) {
                            kinematics_axis_info = itaxisbinding->kinematics_axis_info;
                            motion_axis_info = itaxisbinding->motion_axis_info;
                            bfound = true;
                            break;
                        }
                    }
                    domAxis_constraintRef pdomaxis = vdomaxes[ic];
                    if (!pchildlink) {
                        // create dummy child link
                        // multiple axes can be easily done with "empty links"
                        ROS_WARN_STREAM(str(boost::format("creating dummy link %s, num joints %d\n")%plink->name%numjoints));

                        std::stringstream ss;
                        ss << plink->name;
                        ss <<"_dummy" << numjoints;
                        pchildlink.reset(new Link());
                        pchildlink->name = ss.str();
                        _model->links_.insert(std::make_pair(pchildlink->name,pchildlink));
                    }

                    ROS_DEBUG_STREAM(str(boost::format("Joint %s assigned %d \n")%vjoints[ic]->name%ic));
                    urdf::JointSharedPtr pjoint = vjoints[ic];
                    pjoint->child_link_name = pchildlink->name;

#define PRINT_POSE(pname, apose) ROS_DEBUG(pname" pos: %f %f %f, rot: %f %f %f %f", \
                                           apose.position.x, apose.position.y, apose.position.z, \
                                           apose.rotation.x, apose.rotation.y, apose.rotation.z, apose.rotation.w);
                    {
                      PRINT_POSE("tatt", tatt);
                      PRINT_POSE("trans_joint_to_child", tatt);
                      Pose trans_joint_to_child = _poseFromMatrix(_ExtractFullTransform(pattfull->getLink()));

                      pjoint->parent_to_joint_origin_transform = _poseMult(tatt, trans_joint_to_child);
                      //  Axes and Anchor assignment.
                      Vector3 ax(pdomaxis->getAxis()->getValue()[0],
                                 pdomaxis->getAxis()->getValue()[1],
                                 pdomaxis->getAxis()->getValue()[2]);
                      Pose pinv = _poseInverse(trans_joint_to_child);
                      // rotate axis
                      ax = pinv.rotation * ax;

                      pjoint->axis.x = ax.x;
                      pjoint->axis.y = ax.y;
                      pjoint->axis.z = ax.z;

                      ROS_DEBUG("joint %s axis: %f %f %f -> %f %f %f\n", pjoint->name.c_str(),
                                pdomaxis->getAxis()->getValue()[0],
                                pdomaxis->getAxis()->getValue()[1],
                                pdomaxis->getAxis()->getValue()[2],
                                pjoint->axis.x, pjoint->axis.y, pjoint->axis.z);
                    }

                    if (!motion_axis_info) {
                        ROS_WARN_STREAM(str(boost::format("No motion axis info for joint %s\n")%pjoint->name));
                    }

                    //  Sets the Speed and the Acceleration of the joint
                    if (!!motion_axis_info) {
                        if (!!motion_axis_info->getSpeed()) {
                            pjoint->limits->velocity = resolveFloat(motion_axis_info->getSpeed(),motion_axis_info);
                            ROS_DEBUG("... Joint Speed: %f...\n",pjoint->limits->velocity);
                        }
                        if (!!motion_axis_info->getAcceleration()) {
                            pjoint->limits->effort = resolveFloat(motion_axis_info->getAcceleration(),motion_axis_info);
                            ROS_DEBUG("... Joint effort: %f...\n",pjoint->limits->effort);
                        }
                    }

                    bool joint_locked = false; // if locked, joint angle is static
                    bool kinematics_limits = false;

                    if (!!kinematics_axis_info) {
                        if (!!kinematics_axis_info->getLocked()) {
                            joint_locked = resolveBool(kinematics_axis_info->getLocked(),kinematics_axis_info);
                        }

                        if (joint_locked) { // If joint is locked set limits to the static value.
                            if( pjoint->type == Joint::REVOLUTE || pjoint->type ==Joint::PRISMATIC) {
                                ROS_WARN_STREAM("lock joint!!\n");
                                pjoint->limits->lower = 0;
                                pjoint->limits->upper = 0;
                            }
                        }
                        else if (kinematics_axis_info->getLimits()) { // If there are articulated system kinematics limits
                            kinematics_limits   = true;
                            double fscale = (pjoint->type == Joint::REVOLUTE) ? (M_PI/180.0f) : _GetUnitScale(kinematics_axis_info);
                            if( pjoint->type == Joint::REVOLUTE || pjoint->type ==Joint::PRISMATIC) {
                                pjoint->limits->lower = fscale*(double)(resolveFloat(kinematics_axis_info->getLimits()->getMin(),kinematics_axis_info));
                                pjoint->limits->upper = fscale*(double)(resolveFloat(kinematics_axis_info->getLimits()->getMax(),kinematics_axis_info));
                                if ( pjoint->limits->lower == 0 && pjoint->limits->upper == 0 ) {
                                    pjoint->type = Joint::FIXED;
                                }
                            }
                        }
                    }

                    //  Search limits in the joints section
                    if (!kinematics_axis_info || (!joint_locked && !kinematics_limits)) {
                        //  If there are NO LIMITS
                        if( !pdomaxis->getLimits() ) {
                            ROS_DEBUG_STREAM(str(boost::format("There are NO LIMITS in joint %s:%d ...\n")%pjoint->name%kinematics_limits));
                            if( pjoint->type == Joint::REVOLUTE ) {
                                pjoint->type = Joint::CONTINUOUS; // continuous means revolute?
                                pjoint->limits->lower = -M_PI;
                                pjoint->limits->upper = M_PI;
                            }
                            else {
                                pjoint->limits->lower = -100000;
                                pjoint->limits->upper = 100000;
                            }
                        }
                        else {
                            ROS_DEBUG_STREAM(str(boost::format("There are LIMITS in joint %s ...\n")%pjoint->name));
                            double fscale = (pjoint->type == Joint::REVOLUTE) ? (M_PI/180.0f) : _GetUnitScale(pdomaxis);
                            pjoint->limits->lower = (double)pdomaxis->getLimits()->getMin()->getValue()*fscale;
                            pjoint->limits->upper = (double)pdomaxis->getLimits()->getMax()->getValue()*fscale;
                            if ( pjoint->limits->lower == 0 && pjoint->limits->upper == 0 ) {
                                pjoint->type = Joint::FIXED;
                            }
                        }
                    }

                    if (pjoint->limits->velocity == 0.0) {
                      pjoint->limits->velocity = pjoint->type == Joint::PRISMATIC ? 0.01 : 0.5f;
                    }
                    pchildlink.reset();
                    ++numjoints;
                }
            }
        }

        if( pdomlink->getAttachment_start_array().getCount() > 0 ) {
            ROS_WARN("urdf collada reader does not support attachment_start\n");
        }
        if( pdomlink->getAttachment_end_array().getCount() > 0 ) {
            ROS_WARN("urdf collada reader does not support attachment_end\n");
        }

        plink->visual->geometry = _CreateGeometry(plink->name, listGeomProperties);
        // visual_groups deprecated
        //boost::shared_ptr<std::vector<urdf::VisualSharedPtr > > viss;
        //viss.reset(new std::vector<urdf::VisualSharedPtr >);
        //viss->push_back(plink->visual);
        //plink->visual_groups.insert(std::make_pair("default", viss));

        if( !plink->visual->geometry ) {
          plink->visual.reset();
          plink->collision.reset();
        } else {
          // collision
          plink->collision.reset(new Collision());
          plink->collision->geometry = plink->visual->geometry;
          plink->collision->origin   = plink->visual->origin;
        }

        // collision_groups deprecated
        //boost::shared_ptr<std::vector<urdf::CollisionSharedPtr > > cols;
        //cols.reset(new std::vector<urdf::CollisionSharedPtr >);
        //cols->push_back(plink->collision);
        //plink->collision_groups.insert(std::make_pair("default", cols));

        return plink;
    }

    urdf::GeometrySharedPtr _CreateGeometry(const std::string& name, const std::list<GEOMPROPERTIES>& listGeomProperties)
    {
        std::vector<std::vector<Vector3> > vertices;
        std::vector<std::vector<int> > indices;
        std::vector<Color> ambients;
        std::vector<Color> diffuses;
        unsigned int index, vert_counter;
        vertices.resize(listGeomProperties.size());
        indices.resize(listGeomProperties.size());
        ambients.resize(listGeomProperties.size());
        diffuses.resize(listGeomProperties.size());
        index = 0;
        vert_counter = 0;
        FOREACHC(it, listGeomProperties) {
            vertices[index].resize(it->vertices.size());
            for(size_t i = 0; i < it->vertices.size(); ++i) {
                vertices[index][i] = _poseMult(it->_t, it->vertices[i]);
                vert_counter++;
            }
            indices[index].resize(it->indices.size());
            for(size_t i = 0; i < it->indices.size(); ++i) {
                indices[index][i] = it->indices[i];
            }
            ambients[index] = it->ambientColor;
            diffuses[index] = it->diffuseColor;
// workaround for mesh_loader.cpp:421
// Most of are DAE files don't have ambient color defined
            ambients[index].r *= 0.5; ambients[index].g *= 0.5; ambients[index].b *= 0.5;
            if ( ambients[index].r == 0.0 ) {
                ambients[index].r = 0.0001;
            }
            if ( ambients[index].g == 0.0 ) {
                ambients[index].g = 0.0001;
            }
            if ( ambients[index].b == 0.0 ) {
                ambients[index].b = 0.0001;
            }
            index++;
        }

        if (vert_counter == 0) {
          urdf::MeshSharedPtr ret;
          ret.reset();
          return ret;
        }

        urdf::MeshSharedPtr geometry(new Mesh());
        geometry->type = Geometry::MESH;
        geometry->scale.x = 1;
        geometry->scale.y = 1;
        geometry->scale.z = 1;

        // have to save the geometry into individual collada 1.4 files since URDF does not allow triangle meshes to be specified
        std::stringstream daedata;
        daedata << str(boost::format("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n\
<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">\n\
  <asset>\n\
    <contributor>\n\
      <author>Rosen Diankov</author>\n\
      <comments>\n\
        robot_model/urdf temporary collada geometry\n\
      </comments>\n\
    </contributor>\n\
    <unit name=\"meter\" meter=\"1.0\"/>\n\
    <up_axis>Y_UP</up_axis>\n\
  </asset>\n\
  <library_materials>\n"));
        for(unsigned int i=0; i < index; i++) {
            daedata << str(boost::format("\
    <material id=\"blinn2%d\" name=\"blinn2%d\">\n\
      <instance_effect url=\"#blinn2-fx%d\"/>\n\
    </material>\n")%i%i%i);
        }
        daedata << "\
  </library_materials>\n\
  <library_effects>\n";
        for(unsigned int i=0; i < index; i++) {
            daedata << str(boost::format("\
    <effect id=\"blinn2-fx%d\">\n\
	  <profile_COMMON>\n\
		<technique sid=\"common\">\n\
		  <phong>\n\
			<ambient>\n\
			  <color>%f %f %f %f</color>\n\
			</ambient>\n\
			<diffuse>\n\
			  <color>%f %f %f %f</color>\n\
			</diffuse>\n\
			<specular>\n\
			  <color>0 0 0 1</color>\n\
			</specular>\n\
		  </phong>\n\
		</technique>\n\
      </profile_COMMON>\n\
    </effect>\n")%i%ambients[i].r%ambients[i].g%ambients[i].b%ambients[i].a%diffuses[i].r%diffuses[i].g%diffuses[i].b%diffuses[i].a);
        }
        daedata << str(boost::format("\
  </library_effects>\n\
  <library_geometries>\n"));
        // fill with vertices
        for(unsigned int i=0; i < index; i++) {
            daedata << str(boost::format("\
    <geometry id=\"base2_M1KShape%d\" name=\"base2_M1KShape%d\">\n\
	  <mesh>\n\
		<source id=\"geo0.positions\">\n\
		  <float_array id=\"geo0.positions-array\" count=\"%d\">")%i%i%(vertices[i].size()*3));
            FOREACH(it,vertices[i]) {
                daedata << it->x << " " << it->y << " " << it->z << " ";
            }
            daedata << str(boost::format("\n\
          </float_array>\n\
		  <technique_common>\n\
			<accessor count=\"%d\" source=\"#geo0.positions-array\" stride=\"3\">\n\
			  <param name=\"X\" type=\"float\"/>\n\
			  <param name=\"Y\" type=\"float\"/>\n\
			  <param name=\"Z\" type=\"float\"/>\n\
			</accessor>\n\
		  </technique_common>\n\
		</source>\n\
		<vertices id=\"geo0.vertices\">\n\
		  <input semantic=\"POSITION\" source=\"#geo0.positions\"/>\n\
		</vertices>\n\
		<triangles count=\"%d\" material=\"lambert2SG\">\n\
		  <input offset=\"0\" semantic=\"VERTEX\" source=\"#geo0.vertices\"/>\n\
          <p>")%vertices[i].size()%(indices[i].size()/3));
            // fill with indices
            FOREACH(it,indices[i]) {
                daedata << *it << " ";
            }
            daedata << str(boost::format("</p>\n\
		</triangles>\n\
	  </mesh>\n\
    </geometry>\n"));
        }
        daedata << str(boost::format("\
  </library_geometries>\n\
  <library_visual_scenes>\n\
    <visual_scene id=\"VisualSceneNode\" name=\"base1d_med\">\n\
      <node id=\"%s\" name=\"%s\" type=\"NODE\">\n")%name%name);
        for(unsigned int i=0; i < index; i++) {
            daedata << str(boost::format("\
        <instance_geometry url=\"#base2_M1KShape%i\">\n\
          <bind_material>\n\
            <technique_common>\n\
              <instance_material symbol=\"lambert2SG\" target=\"#blinn2%i\"/>\n\
            </technique_common>\n\
          </bind_material>\n\
        </instance_geometry>\n")%i%i);
        }
        daedata << str(boost::format("\
      </node>\n\
    </visual_scene>\n\
  </library_visual_scenes>\n\
  <scene>\n\
    <instance_visual_scene url=\"#VisualSceneNode\"/>\n\
  </scene>\n\
</COLLADA>"));

#ifdef HAVE_MKSTEMPS
        geometry->filename = str(boost::format("/tmp/collada_model_reader_%s_XXXXXX.dae")%name);
        int fd = mkstemps(&geometry->filename[0],4);
#else
        int fd = -1;
        for(int iter = 0; iter < 1000; ++iter) {
            geometry->filename = str(boost::format("/tmp/collada_model_reader_%s_%d.dae")%name%rand());
            if( !!std::ifstream(geometry->filename.c_str())) {
                fd = open(geometry->filename.c_str(),O_WRONLY|O_CREAT|O_EXCL);
                if( fd != -1 ) {
                    break;
                }
            }
        }
        if( fd == -1 ) {
            ROS_ERROR("failed to open geometry dae file %s",geometry->filename.c_str());
            return geometry;
        }
#endif
        //ROS_INFO("temp file: %s",geometry->filename.c_str());
        std::string daedatastr = daedata.str();
        if( (size_t)write(fd,daedatastr.c_str(),daedatastr.size()) != daedatastr.size() ) {
            ROS_ERROR("failed to write to geometry dae file %s",geometry->filename.c_str());
        }
        close(fd);
        _listTempFilenames.push_back(boost::shared_ptr<UnlinkFilename>(new UnlinkFilename(geometry->filename)));
        geometry->filename = std::string("file://") + geometry->filename;
        return geometry;
    }

    /// Extract Geometry and apply the transformations of the node
    /// \param pdomnode Node to extract the goemetry
    /// \param plink    Link of the kinematics model
    void _ExtractGeometry(const domNodeRef pdomnode,std::list<GEOMPROPERTIES>& listGeomProperties, const std::list<JointAxisBinding>& listAxisBindings, const Pose& tlink)
    {
        if( !pdomnode ) {
            return;
        }

        ROS_DEBUG_STREAM(str(boost::format("ExtractGeometry(node,link) of %s\n")%pdomnode->getName()));

        // For all child nodes of pdomnode
        for (size_t i = 0; i < pdomnode->getNode_array().getCount(); i++) {
            // check if contains a joint
            bool contains=false;
            FOREACHC(it,listAxisBindings) {
                // don't check ID's check if the reference is the same!
                if ( (pdomnode->getNode_array()[i])  == (it->visualnode)) {
                    contains=true;
                    break;
                }
            }
            if (contains) {
                continue;
            }

            _ExtractGeometry(pdomnode->getNode_array()[i],listGeomProperties, listAxisBindings,tlink);
            // Plink stayes the same for all children
            // replace pdomnode by child = pdomnode->getNode_array()[i]
            // hope for the best!
            // put everything in a subroutine in order to process pdomnode too!
        }

        unsigned int nGeomBefore =  listGeomProperties.size(); // #of Meshes already associated to this link

        // get the geometry
        for (size_t igeom = 0; igeom < pdomnode->getInstance_geometry_array().getCount(); ++igeom) {
            domInstance_geometryRef domigeom = pdomnode->getInstance_geometry_array()[igeom];
            domGeometryRef domgeom = daeSafeCast<domGeometry> (domigeom->getUrl().getElement());
            if (!domgeom) {
                continue;
            }

            //  Gets materials
            std::map<std::string, domMaterialRef> mapmaterials;
            if (!!domigeom->getBind_material() && !!domigeom->getBind_material()->getTechnique_common()) {
                const domInstance_material_Array& matarray = domigeom->getBind_material()->getTechnique_common()->getInstance_material_array();
                for (size_t imat = 0; imat < matarray.getCount(); ++imat) {
                    domMaterialRef pmat = daeSafeCast<domMaterial>(matarray[imat]->getTarget().getElement());
                    if (!!pmat) {
                        mapmaterials[matarray[imat]->getSymbol()] = pmat;
                    }
                }
            }

            //  Gets the geometry
            _ExtractGeometry(domgeom, mapmaterials, listGeomProperties);
        }

        std::list<GEOMPROPERTIES>::iterator itgeom= listGeomProperties.begin();
        for (unsigned int i=0; i< nGeomBefore; i++) {
            itgeom++; // change only the transformations of the newly found geometries.
        }

        boost::array<double,12> tmnodegeom = _poseMult(_matrixFromPose(_poseInverse(tlink)),
                                                       _poseMult(_poseMult(_matrixFromPose(_poseInverse(_VisualRootOrigin)),
                                                                           _getNodeParentTransform(pdomnode)),
                                                                 _ExtractFullTransform(pdomnode)));
        Pose tnodegeom;
        Vector3 vscale(1,1,1);
        _decompose(tmnodegeom, tnodegeom, vscale);

        ROS_DEBUG_STREAM("tnodegeom: " << pdomnode->getName()
                         << tnodegeom.position.x << " " << tnodegeom.position.y << " " << tnodegeom.position.z << " / "
                         << tnodegeom.rotation.x << " " << tnodegeom.rotation.y << " " << tnodegeom.rotation.z << " "
                         << tnodegeom.rotation.w);

        //        std::stringstream ss; ss << "geom: ";
        //        for(int i = 0; i < 4; ++i) {
        //            ss << tmnodegeom[4*0+i] << " " << tmnodegeom[4*1+i] << " " << tmnodegeom[4*2+i] << " ";
        //        }
        //        ROS_INFO(ss.str().c_str());

        //  Switch between different type of geometry PRIMITIVES
        for (; itgeom != listGeomProperties.end(); itgeom++) {
            itgeom->_t = tnodegeom;
            switch (itgeom->type) {
            case GeomBox:
                itgeom->vGeomData.x *= vscale.x;
                itgeom->vGeomData.y *= vscale.y;
                itgeom->vGeomData.z *= vscale.z;
                break;
            case GeomSphere: {
                itgeom->vGeomData.x *= std::max(vscale.z, std::max(vscale.x, vscale.y));
                break;
            }
            case GeomCylinder:
                itgeom->vGeomData.x *= std::max(vscale.x, vscale.y);
                itgeom->vGeomData.y *= vscale.z;
                break;
            case GeomTrimesh:
                for(size_t i = 0; i < itgeom->vertices.size(); ++i ) {
                    itgeom->vertices[i] = _poseMult(tmnodegeom,itgeom->vertices[i]);
                }
                itgeom->_t = Pose(); // reset back to identity
                break;
            default:
                ROS_WARN_STREAM(str(boost::format("unknown geometry type: %d\n")%itgeom->type));
            }
        }
    }

    /// Paint the Geometry with the color material
    /// \param  pmat    Material info of the COLLADA's model
    /// \param  geom    Geometry properties in OpenRAVE
    void _FillGeometryColor(const domMaterialRef pmat, GEOMPROPERTIES& geom)
    {
        if( !!pmat && !!pmat->getInstance_effect() ) {
            domEffectRef peffect = daeSafeCast<domEffect>(pmat->getInstance_effect()->getUrl().getElement().cast());
            if( !!peffect ) {
                domProfile_common::domTechnique::domPhongRef pphong = daeSafeCast<domProfile_common::domTechnique::domPhong>(peffect->getDescendant(daeElement::matchType(domProfile_common::domTechnique::domPhong::ID())));
                if( !!pphong ) {
                    if( !!pphong->getAmbient() && !!pphong->getAmbient()->getColor() ) {
                        domFx_color c = pphong->getAmbient()->getColor()->getValue();
                        geom.ambientColor.r = c[0];
                        geom.ambientColor.g = c[1];
                        geom.ambientColor.b = c[2];
                        geom.ambientColor.a = c[3];
                    }
                    if( !!pphong->getDiffuse() && !!pphong->getDiffuse()->getColor() ) {
                        domFx_color c = pphong->getDiffuse()->getColor()->getValue();
                        geom.diffuseColor.r = c[0];
                        geom.diffuseColor.g = c[1];
                        geom.diffuseColor.b = c[2];
                        geom.diffuseColor.a = c[3];
                    }
                }
            }
        }
    }

    /// Extract the Geometry in TRIANGLES and adds it to OpenRave
    /// \param  triRef  Array of triangles of the COLLADA's model
    /// \param  vertsRef    Array of vertices of the COLLADA's model
    /// \param  mapmaterials    Materials applied to the geometry
    /// \param  plink   Link of the kinematics model
    bool _ExtractGeometry(const domTrianglesRef triRef, const domVerticesRef vertsRef, const std::map<std::string,domMaterialRef>& mapmaterials, std::list<GEOMPROPERTIES>& listGeomProperties)
    {
        if( !triRef ) {
            return false;
        }
        listGeomProperties.push_back(GEOMPROPERTIES());
        GEOMPROPERTIES& geom = listGeomProperties.back();
        geom.type = GeomTrimesh;

        // resolve the material and assign correct colors to the geometry
        if( !!triRef->getMaterial() ) {
            std::map<std::string,domMaterialRef>::const_iterator itmat = mapmaterials.find(triRef->getMaterial());
            if( itmat != mapmaterials.end() ) {
                _FillGeometryColor(itmat->second,geom);
            }
        }

        size_t triangleIndexStride = 0, vertexoffset = -1;
        domInput_local_offsetRef indexOffsetRef;

        for (unsigned int w=0; w<triRef->getInput_array().getCount(); w++) {
            size_t offset = triRef->getInput_array()[w]->getOffset();
            daeString str = triRef->getInput_array()[w]->getSemantic();
            if (!strcmp(str,"VERTEX")) {
                indexOffsetRef = triRef->getInput_array()[w];
                vertexoffset = offset;
            }
            if (offset> triangleIndexStride) {
                triangleIndexStride = offset;
            }
        }
        triangleIndexStride++;

        const domList_of_uints& indexArray =triRef->getP()->getValue();
        geom.indices.reserve(triRef->getCount()*3);
        geom.vertices.reserve(triRef->getCount()*3);
        for (size_t i=0; i<vertsRef->getInput_array().getCount(); ++i) {
            domInput_localRef localRef = vertsRef->getInput_array()[i];
            daeString str = localRef->getSemantic();
            if ( strcmp(str,"POSITION") == 0 ) {
                const domSourceRef node = daeSafeCast<domSource>(localRef->getSource().getElement());
                if( !node ) {
                    continue;
                }
                double fUnitScale = _GetUnitScale(node);
                const domFloat_arrayRef flArray = node->getFloat_array();
                if (!!flArray) {
                    const domList_of_floats& listFloats = flArray->getValue();
                    int k=vertexoffset;
                    int vertexStride = 3; //instead of hardcoded stride, should use the 'accessor'
                    for(size_t itri = 0; itri < triRef->getCount(); ++itri) {
                        if(k+2*triangleIndexStride < indexArray.getCount() ) {
                            for (int j=0; j<3; j++) {
                                int index0 = indexArray.get(k)*vertexStride;
                                domFloat fl0 = listFloats.get(index0);
                                domFloat fl1 = listFloats.get(index0+1);
                                domFloat fl2 = listFloats.get(index0+2);
                                k+=triangleIndexStride;
                                geom.indices.push_back(geom.vertices.size());
                                geom.vertices.push_back(Vector3(fl0*fUnitScale,fl1*fUnitScale,fl2*fUnitScale));
                            }
                        }
                    }
                }
                else {
                    ROS_WARN_STREAM("float array not defined!\n");
                }
                break;
            }
        }
        if( geom.indices.size() != 3*triRef->getCount() ) {
            ROS_WARN_STREAM("triangles declares wrong count!\n");
        }
        geom.InitCollisionMesh();
        return true;
    }

    /// Extract the Geometry in TRIGLE FANS and adds it to OpenRave
    /// \param  triRef  Array of triangle fans of the COLLADA's model
    /// \param  vertsRef    Array of vertices of the COLLADA's model
    /// \param  mapmaterials    Materials applied to the geometry
    bool _ExtractGeometry(const domTrifansRef triRef, const domVerticesRef vertsRef, const std::map<std::string,domMaterialRef>& mapmaterials, std::list<GEOMPROPERTIES>& listGeomProperties)
    {
        if( !triRef ) {
            return false;
        }
        listGeomProperties.push_back(GEOMPROPERTIES());
        GEOMPROPERTIES& geom = listGeomProperties.back();
        geom.type = GeomTrimesh;

        // resolve the material and assign correct colors to the geometry
        if( !!triRef->getMaterial() ) {
            std::map<std::string,domMaterialRef>::const_iterator itmat = mapmaterials.find(triRef->getMaterial());
            if( itmat != mapmaterials.end() ) {
                _FillGeometryColor(itmat->second,geom);
            }
        }

        size_t triangleIndexStride = 0, vertexoffset = -1;
        domInput_local_offsetRef indexOffsetRef;

        for (unsigned int w=0; w<triRef->getInput_array().getCount(); w++) {
            size_t offset = triRef->getInput_array()[w]->getOffset();
            daeString str = triRef->getInput_array()[w]->getSemantic();
            if (!strcmp(str,"VERTEX")) {
                indexOffsetRef = triRef->getInput_array()[w];
                vertexoffset = offset;
            }
            if (offset> triangleIndexStride) {
                triangleIndexStride = offset;
            }
        }
        triangleIndexStride++;
        size_t primitivecount = triRef->getCount();
        if( primitivecount > triRef->getP_array().getCount() ) {
            ROS_WARN_STREAM("trifans has incorrect count\n");
            primitivecount = triRef->getP_array().getCount();
        }
        for(size_t ip = 0; ip < primitivecount; ++ip) {
            domList_of_uints indexArray =triRef->getP_array()[ip]->getValue();
            for (size_t i=0; i<vertsRef->getInput_array().getCount(); ++i) {
                domInput_localRef localRef = vertsRef->getInput_array()[i];
                daeString str = localRef->getSemantic();
                if ( strcmp(str,"POSITION") == 0 ) {
                    const domSourceRef node = daeSafeCast<domSource>(localRef->getSource().getElement());
                    if( !node ) {
                        continue;
                    }
                    double fUnitScale = _GetUnitScale(node);
                    const domFloat_arrayRef flArray = node->getFloat_array();
                    if (!!flArray) {
                        const domList_of_floats& listFloats = flArray->getValue();
                        int k=vertexoffset;
                        int vertexStride = 3; //instead of hardcoded stride, should use the 'accessor'
                        size_t usedindices = 3*(indexArray.getCount()-2);
                        if( geom.indices.capacity() < geom.indices.size()+usedindices ) {
                            geom.indices.reserve(geom.indices.size()+usedindices);
                        }
                        if( geom.vertices.capacity() < geom.vertices.size()+indexArray.getCount() ) {
                            geom.vertices.reserve(geom.vertices.size()+indexArray.getCount());
                        }
                        size_t startoffset = (int)geom.vertices.size();
                        while(k < (int)indexArray.getCount() ) {
                            int index0 = indexArray.get(k)*vertexStride;
                            domFloat fl0 = listFloats.get(index0);
                            domFloat fl1 = listFloats.get(index0+1);
                            domFloat fl2 = listFloats.get(index0+2);
                            k+=triangleIndexStride;
                            geom.vertices.push_back(Vector3(fl0*fUnitScale,fl1*fUnitScale,fl2*fUnitScale));
                        }
                        for(size_t ivert = startoffset+2; ivert < geom.vertices.size(); ++ivert) {
                            geom.indices.push_back(startoffset);
                            geom.indices.push_back(ivert-1);
                            geom.indices.push_back(ivert);
                        }
                    }
                    else {
                        ROS_WARN_STREAM("float array not defined!\n");
                    }
                    break;
                }
            }
        }

        geom.InitCollisionMesh();
        return false;
    }

    /// Extract the Geometry in TRIANGLE STRIPS and adds it to OpenRave
    /// \param  triRef  Array of Triangle Strips of the COLLADA's model
    /// \param  vertsRef    Array of vertices of the COLLADA's model
    /// \param  mapmaterials    Materials applied to the geometry
    bool _ExtractGeometry(const domTristripsRef triRef, const domVerticesRef vertsRef, const std::map<std::string,domMaterialRef>& mapmaterials, std::list<GEOMPROPERTIES>& listGeomProperties)
    {
        if( !triRef ) {
            return false;
        }
        listGeomProperties.push_back(GEOMPROPERTIES());
        GEOMPROPERTIES& geom = listGeomProperties.back();
        geom.type = GeomTrimesh;

        // resolve the material and assign correct colors to the geometry
        if( !!triRef->getMaterial() ) {
            std::map<std::string,domMaterialRef>::const_iterator itmat = mapmaterials.find(triRef->getMaterial());
            if( itmat != mapmaterials.end() ) {
                _FillGeometryColor(itmat->second,geom);
            }
        }
        size_t triangleIndexStride = 0, vertexoffset = -1;
        domInput_local_offsetRef indexOffsetRef;

        for (unsigned int w=0; w<triRef->getInput_array().getCount(); w++) {
            size_t offset = triRef->getInput_array()[w]->getOffset();
            daeString str = triRef->getInput_array()[w]->getSemantic();
            if (!strcmp(str,"VERTEX")) {
                indexOffsetRef = triRef->getInput_array()[w];
                vertexoffset = offset;
            }
            if (offset> triangleIndexStride) {
                triangleIndexStride = offset;
            }
        }
        triangleIndexStride++;
        size_t primitivecount = triRef->getCount();
        if( primitivecount > triRef->getP_array().getCount() ) {
            ROS_WARN_STREAM("tristrips has incorrect count\n");
            primitivecount = triRef->getP_array().getCount();
        }
        for(size_t ip = 0; ip < primitivecount; ++ip) {
            domList_of_uints indexArray = triRef->getP_array()[ip]->getValue();
            for (size_t i=0; i<vertsRef->getInput_array().getCount(); ++i) {
                domInput_localRef localRef = vertsRef->getInput_array()[i];
                daeString str = localRef->getSemantic();
                if ( strcmp(str,"POSITION") == 0 ) {
                    const domSourceRef node = daeSafeCast<domSource>(localRef->getSource().getElement());
                    if( !node ) {
                        continue;
                    }
                    double fUnitScale = _GetUnitScale(node);
                    const domFloat_arrayRef flArray = node->getFloat_array();
                    if (!!flArray) {
                        const domList_of_floats& listFloats = flArray->getValue();
                        int k=vertexoffset;
                        int vertexStride = 3; //instead of hardcoded stride, should use the 'accessor'
                        size_t usedindices = indexArray.getCount()-2;
                        if( geom.indices.capacity() < geom.indices.size()+usedindices ) {
                            geom.indices.reserve(geom.indices.size()+usedindices);
                        }
                        if( geom.vertices.capacity() < geom.vertices.size()+indexArray.getCount() ) {
                            geom.vertices.reserve(geom.vertices.size()+indexArray.getCount());
                        }

                        size_t startoffset = (int)geom.vertices.size();
                        while(k < (int)indexArray.getCount() ) {
                            int index0 = indexArray.get(k)*vertexStride;
                            domFloat fl0 = listFloats.get(index0);
                            domFloat fl1 = listFloats.get(index0+1);
                            domFloat fl2 = listFloats.get(index0+2);
                            k+=triangleIndexStride;
                            geom.vertices.push_back(Vector3(fl0*fUnitScale,fl1*fUnitScale,fl2*fUnitScale));
                        }

                        bool bFlip = false;
                        for(size_t ivert = startoffset+2; ivert < geom.vertices.size(); ++ivert) {
                            geom.indices.push_back(ivert-2);
                            geom.indices.push_back(bFlip ? ivert : ivert-1);
                            geom.indices.push_back(bFlip ? ivert-1 : ivert);
                            bFlip = !bFlip;
                        }
                    }
                    else {
                        ROS_WARN_STREAM("float array not defined!\n");
                    }
                    break;
                }
            }
        }

        geom.InitCollisionMesh();
        return false;
    }

    /// Extract the Geometry in TRIANGLE STRIPS and adds it to OpenRave
    /// \param  triRef  Array of Triangle Strips of the COLLADA's model
    /// \param  vertsRef    Array of vertices of the COLLADA's model
    /// \param  mapmaterials    Materials applied to the geometry
    bool _ExtractGeometry(const domPolylistRef triRef, const domVerticesRef vertsRef, const std::map<std::string,domMaterialRef>& mapmaterials, std::list<GEOMPROPERTIES>& listGeomProperties)
    {
        if( !triRef ) {
            return false;
        }
        listGeomProperties.push_back(GEOMPROPERTIES());
        GEOMPROPERTIES& geom = listGeomProperties.back();
        geom.type = GeomTrimesh;

        // resolve the material and assign correct colors to the geometry
        if( !!triRef->getMaterial() ) {
            std::map<std::string,domMaterialRef>::const_iterator itmat = mapmaterials.find(triRef->getMaterial());
            if( itmat != mapmaterials.end() ) {
                _FillGeometryColor(itmat->second,geom);
            }
        }

        size_t triangleIndexStride = 0,vertexoffset = -1;
        domInput_local_offsetRef indexOffsetRef;
        for (unsigned int w=0; w<triRef->getInput_array().getCount(); w++) {
            size_t offset = triRef->getInput_array()[w]->getOffset();
            daeString str = triRef->getInput_array()[w]->getSemantic();
            if (!strcmp(str,"VERTEX")) {
                indexOffsetRef = triRef->getInput_array()[w];
                vertexoffset = offset;
            }
            if (offset> triangleIndexStride) {
                triangleIndexStride = offset;
            }
        }
        triangleIndexStride++;
        const domList_of_uints& indexArray =triRef->getP()->getValue();
        for (size_t i=0; i<vertsRef->getInput_array().getCount(); ++i) {
            domInput_localRef localRef = vertsRef->getInput_array()[i];
            daeString str = localRef->getSemantic();
            if ( strcmp(str,"POSITION") == 0 ) {
                const domSourceRef node = daeSafeCast<domSource>(localRef->getSource().getElement());
                if( !node ) {
                    continue;
                }
                double fUnitScale = _GetUnitScale(node);
                const domFloat_arrayRef flArray = node->getFloat_array();
                if (!!flArray) {
                    const domList_of_floats& listFloats = flArray->getValue();
                    size_t k=vertexoffset;
                    int vertexStride = 3; //instead of hardcoded stride, should use the 'accessor'
                    for(size_t ipoly = 0; ipoly < triRef->getVcount()->getValue().getCount(); ++ipoly) {
                        size_t numverts = triRef->getVcount()->getValue()[ipoly];
                        if(numverts > 0 && k+(numverts-1)*triangleIndexStride < indexArray.getCount() ) {
                            size_t startoffset = geom.vertices.size();
                            for (size_t j=0; j<numverts; j++) {
                                int index0 = indexArray.get(k)*vertexStride;
                                domFloat fl0 = listFloats.get(index0);
                                domFloat fl1 = listFloats.get(index0+1);
                                domFloat fl2 = listFloats.get(index0+2);
                                k+=triangleIndexStride;
                                geom.vertices.push_back(Vector3(fl0*fUnitScale,fl1*fUnitScale,fl2*fUnitScale));
                            }
                            for(size_t ivert = startoffset+2; ivert < geom.vertices.size(); ++ivert) {
                                geom.indices.push_back(startoffset);
                                geom.indices.push_back(ivert-1);
                                geom.indices.push_back(ivert);
                            }
                        }
                    }
                }
                else {
                    ROS_WARN_STREAM("float array not defined!\n");
                }
                break;
            }
        }
        geom.InitCollisionMesh();
        return false;
    }

    /// Extract the Geometry and adds it to OpenRave
    /// \param  geom    Geometry to extract of the COLLADA's model
    /// \param  mapmaterials    Materials applied to the geometry
    bool _ExtractGeometry(const domGeometryRef geom, const std::map<std::string,domMaterialRef>& mapmaterials, std::list<GEOMPROPERTIES>& listGeomProperties)
    {
        if( !geom ) {
            return false;
        }
        std::vector<Vector3> vconvexhull;
        if (geom->getMesh()) {
            const domMeshRef meshRef = geom->getMesh();
            for (size_t tg = 0; tg<meshRef->getTriangles_array().getCount(); tg++) {
                _ExtractGeometry(meshRef->getTriangles_array()[tg], meshRef->getVertices(), mapmaterials, listGeomProperties);
            }
            for (size_t tg = 0; tg<meshRef->getTrifans_array().getCount(); tg++) {
                _ExtractGeometry(meshRef->getTrifans_array()[tg], meshRef->getVertices(), mapmaterials, listGeomProperties);
            }
            for (size_t tg = 0; tg<meshRef->getTristrips_array().getCount(); tg++) {
                _ExtractGeometry(meshRef->getTristrips_array()[tg], meshRef->getVertices(), mapmaterials, listGeomProperties);
            }
            for (size_t tg = 0; tg<meshRef->getPolylist_array().getCount(); tg++) {
                _ExtractGeometry(meshRef->getPolylist_array()[tg], meshRef->getVertices(), mapmaterials, listGeomProperties);
            }
            if( meshRef->getPolygons_array().getCount()> 0 ) {
                ROS_WARN_STREAM("openrave does not support collada polygons\n");
            }

            //            if( alltrimesh.vertices.size() == 0 ) {
            //                const domVerticesRef vertsRef = meshRef->getVertices();
            //                for (size_t i=0;i<vertsRef->getInput_array().getCount();i++) {
            //                    domInput_localRef localRef = vertsRef->getInput_array()[i];
            //                    daeString str = localRef->getSemantic();
            //                    if ( strcmp(str,"POSITION") == 0 ) {
            //                        const domSourceRef node = daeSafeCast<domSource>(localRef->getSource().getElement());
            //                        if( !node )
            //                            continue;
            //                        double fUnitScale = _GetUnitScale(node);
            //                        const domFloat_arrayRef flArray = node->getFloat_array();
            //                        if (!!flArray) {
            //                            const domList_of_floats& listFloats = flArray->getValue();
            //                            int vertexStride = 3;//instead of hardcoded stride, should use the 'accessor'
            //                            vconvexhull.reserve(vconvexhull.size()+listFloats.getCount());
            //                            for (size_t vertIndex = 0;vertIndex < listFloats.getCount();vertIndex+=vertexStride) {
            //                                //btVector3 verts[3];
            //                                domFloat fl0 = listFloats.get(vertIndex);
            //                                domFloat fl1 = listFloats.get(vertIndex+1);
            //                                domFloat fl2 = listFloats.get(vertIndex+2);
            //                                vconvexhull.push_back(Vector3(fl0*fUnitScale,fl1*fUnitScale,fl2*fUnitScale));
            //                            }
            //                        }
            //                    }
            //                }
            //
            //                _computeConvexHull(vconvexhull,alltrimesh);
            //            }

            return true;
        }
        else if (geom->getConvex_mesh()) {
            {
                const domConvex_meshRef convexRef = geom->getConvex_mesh();
                daeElementRef otherElemRef = convexRef->getConvex_hull_of().getElement();
                if ( !!otherElemRef ) {
                    domGeometryRef linkedGeom = *(domGeometryRef*)&otherElemRef;
                    ROS_WARN_STREAM( "otherLinked\n");
                }
                else {
                    ROS_WARN("convexMesh polyCount = %d\n",(int)convexRef->getPolygons_array().getCount());
                    ROS_WARN("convexMesh triCount = %d\n",(int)convexRef->getTriangles_array().getCount());
                }
            }

            const domConvex_meshRef convexRef = geom->getConvex_mesh();
            //daeString urlref = convexRef->getConvex_hull_of().getURI();
            daeString urlref2 = convexRef->getConvex_hull_of().getOriginalURI();
            if (urlref2) {
                daeElementRef otherElemRef = convexRef->getConvex_hull_of().getElement();

                // Load all the geometry libraries
                for ( size_t i = 0; i < _dom->getLibrary_geometries_array().getCount(); i++) {
                    domLibrary_geometriesRef libgeom = _dom->getLibrary_geometries_array()[i];
                    for (size_t i = 0; i < libgeom->getGeometry_array().getCount(); i++) {
                        domGeometryRef lib = libgeom->getGeometry_array()[i];
                        if (!strcmp(lib->getId(),urlref2+1)) { // skip the # at the front of urlref2
                            //found convex_hull geometry
                            domMesh *meshElement = lib->getMesh(); //linkedGeom->getMesh();
                            if (meshElement) {
                                const domVerticesRef vertsRef = meshElement->getVertices();
                                for (size_t i=0; i<vertsRef->getInput_array().getCount(); i++) {
                                    domInput_localRef localRef = vertsRef->getInput_array()[i];
                                    daeString str = localRef->getSemantic();
                                    if ( strcmp(str,"POSITION") == 0) {
                                        const domSourceRef node = daeSafeCast<domSource>(localRef->getSource().getElement());
                                        if( !node ) {
                                            continue;
                                        }
                                        double fUnitScale = _GetUnitScale(node);
                                        const domFloat_arrayRef flArray = node->getFloat_array();
                                        if (!!flArray) {
                                            vconvexhull.reserve(vconvexhull.size()+flArray->getCount());
                                            const domList_of_floats& listFloats = flArray->getValue();
                                            for (size_t k=0; k+2<flArray->getCount(); k+=3) {
                                                domFloat fl0 = listFloats.get(k);
                                                domFloat fl1 = listFloats.get(k+1);
                                                domFloat fl2 = listFloats.get(k+2);
                                                vconvexhull.push_back(Vector3(fl0*fUnitScale,fl1*fUnitScale,fl2*fUnitScale));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else {
                //no getConvex_hull_of but direct vertices
                const domVerticesRef vertsRef = convexRef->getVertices();
                for (size_t i=0; i<vertsRef->getInput_array().getCount(); i++) {
                    domInput_localRef localRef = vertsRef->getInput_array()[i];
                    daeString str = localRef->getSemantic();
                    if ( strcmp(str,"POSITION") == 0 ) {
                        const domSourceRef node = daeSafeCast<domSource>(localRef->getSource().getElement());
                        if( !node ) {
                            continue;
                        }
                        double fUnitScale = _GetUnitScale(node);
                        const domFloat_arrayRef flArray = node->getFloat_array();
                        if (!!flArray) {
                            const domList_of_floats& listFloats = flArray->getValue();
                            vconvexhull.reserve(vconvexhull.size()+flArray->getCount());
                            for (size_t k=0; k+2<flArray->getCount(); k+=3) {
                                domFloat fl0 = listFloats.get(k);
                                domFloat fl1 = listFloats.get(k+1);
                                domFloat fl2 = listFloats.get(k+2);
                                vconvexhull.push_back(Vector3(fl0*fUnitScale,fl1*fUnitScale,fl2*fUnitScale));
                            }
                        }
                    }
                }
            }

            if( vconvexhull.size()> 0 ) {
                listGeomProperties.push_back(GEOMPROPERTIES());
                GEOMPROPERTIES& geom = listGeomProperties.back();
                geom.type = GeomTrimesh;

                //_computeConvexHull(vconvexhull,trimesh);
                geom.InitCollisionMesh();
            }
            return true;
        }

        return false;
    }

    /// \brief extract the robot actuators
    void _ExtractRobotAttachedActuators(const domArticulated_systemRef as)
    {
        // over write joint parameters by elements in instance_actuator
        for(size_t ie = 0; ie < as->getExtra_array().getCount(); ++ie) {
            domExtraRef pextra = as->getExtra_array()[ie];
            if( strcmp(pextra->getType(), "attach_actuator") == 0 ) {
                //std::string aname = pextra->getAttribute("name");
                domTechniqueRef tec = _ExtractOpenRAVEProfile(pextra->getTechnique_array());
                if( !!tec ) {
                    urdf::JointSharedPtr pjoint;
                    daeElementRef domactuator;
                    {
                        daeElementRef bact = tec->getChild("bind_actuator");
                        pjoint = _getJointFromRef(bact->getAttribute("joint").c_str(), as);
                        if (!pjoint) continue;
                    }
                    {
                        daeElementRef iact = tec->getChild("instance_actuator");
                        if(!iact) continue;
                        std::string instance_url = iact->getAttribute("url");
                        domactuator = daeURI(*iact, instance_url).getElement();
                        if(!domactuator) continue;
                    }
                    daeElement *nom_torque = domactuator->getChild("nominal_torque");
                    if ( !! nom_torque ) {
                        if( !! pjoint->limits ) {
                            pjoint->limits->effort = boost::lexical_cast<double>(nom_torque->getCharData());
                            ROS_DEBUG("effort limit at joint (%s) is over written by %f",
                                      pjoint->name.c_str(), pjoint->limits->effort);
                        }
                    }
                }
            }
        }
    }

    /// \brief extract the robot manipulators
    void _ExtractRobotManipulators(const domArticulated_systemRef as)
    {
        ROS_DEBUG("collada manipulators not supported yet");
    }

    /// \brief Extract Sensors attached to a Robot
    void _ExtractRobotAttachedSensors(const domArticulated_systemRef as)
    {
        ROS_DEBUG("collada sensors not supported yet");
    }

    inline daeElementRef _getElementFromUrl(const daeURI &uri)
    {
        return daeStandardURIResolver(*_collada).resolveElement(uri);
    }

    static daeElement* searchBinding(domCommon_sidref_or_paramRef paddr, daeElementRef parent)
    {
        if( !!paddr->getSIDREF() ) {
            return daeSidRef(paddr->getSIDREF()->getValue(),parent).resolve().elt;
        }
        if (!!paddr->getParam()) {
            return searchBinding(paddr->getParam()->getValue(),parent);
        }
        return NULL;
    }

    /// Search a given parameter reference and stores the new reference to search.
    /// \param ref the reference name to search
    /// \param parent The array of parameter where the method searchs.
    static daeElement* searchBinding(daeString ref, daeElementRef parent)
    {
        if( !parent ) {
            return NULL;
        }
        daeElement* pelt = NULL;
        domKinematics_sceneRef kscene = daeSafeCast<domKinematics_scene>(parent.cast());
        if( !!kscene ) {
            pelt = searchBindingArray(ref,kscene->getInstance_articulated_system_array());
            if( !!pelt ) {
                return pelt;
            }
            return searchBindingArray(ref,kscene->getInstance_kinematics_model_array());
        }
        domArticulated_systemRef articulated_system = daeSafeCast<domArticulated_system>(parent.cast());
        if( !!articulated_system ) {
            if( !!articulated_system->getKinematics() ) {
                pelt = searchBindingArray(ref,articulated_system->getKinematics()->getInstance_kinematics_model_array());
                if( !!pelt ) {
                    return pelt;
                }
            }
            if( !!articulated_system->getMotion() ) {
                return searchBinding(ref,articulated_system->getMotion()->getInstance_articulated_system());
            }
            return NULL;
        }
        // try to get a bind array
        daeElementRef pbindelt;
        const domKinematics_bind_Array* pbindarray = NULL;
        const domKinematics_newparam_Array* pnewparamarray = NULL;
        domInstance_articulated_systemRef ias = daeSafeCast<domInstance_articulated_system>(parent.cast());
        if( !!ias ) {
            pbindarray = &ias->getBind_array();
            pbindelt = ias->getUrl().getElement();
            pnewparamarray = &ias->getNewparam_array();
        }
        if( !pbindarray || !pbindelt ) {
            domInstance_kinematics_modelRef ikm = daeSafeCast<domInstance_kinematics_model>(parent.cast());
            if( !!ikm ) {
                pbindarray = &ikm->getBind_array();
                pbindelt = ikm->getUrl().getElement();
                pnewparamarray = &ikm->getNewparam_array();
            }
        }
        if( !!pbindarray && !!pbindelt ) {
            for (size_t ibind = 0; ibind < pbindarray->getCount(); ++ibind) {
                domKinematics_bindRef pbind = (*pbindarray)[ibind];
                if( !!pbind->getSymbol() && strcmp(pbind->getSymbol(), ref) == 0 ) {
                    // found a match
                    if( !!pbind->getParam() ) {
                        return daeSidRef(pbind->getParam()->getRef(), pbindelt).resolve().elt;
                    }
                    else if( !!pbind->getSIDREF() ) {
                        return daeSidRef(pbind->getSIDREF()->getValue(), pbindelt).resolve().elt;
                    }
                }
            }
            for(size_t inewparam = 0; inewparam < pnewparamarray->getCount(); ++inewparam) {
                domKinematics_newparamRef newparam = (*pnewparamarray)[inewparam];
                if( !!newparam->getSid() && strcmp(newparam->getSid(), ref) == 0 ) {
                    if( !!newparam->getSIDREF() ) { // can only bind with SIDREF
                        return daeSidRef(newparam->getSIDREF()->getValue(),pbindelt).resolve().elt;
                    }
                    ROS_WARN_STREAM(str(boost::format("newparam sid=%s does not have SIDREF\n")%newparam->getSid()));
                }
            }
        }
        ROS_WARN_STREAM(str(boost::format("failed to get binding '%s' for element: %s\n")%ref%parent->getElementName()));
        return NULL;
    }

    static daeElement* searchBindingArray(daeString ref, const domInstance_articulated_system_Array& paramArray)
    {
        for(size_t iikm = 0; iikm < paramArray.getCount(); ++iikm) {
            daeElement* pelt = searchBinding(ref,paramArray[iikm].cast());
            if( !!pelt ) {
                return pelt;
            }
        }
        return NULL;
    }

    static daeElement* searchBindingArray(daeString ref, const domInstance_kinematics_model_Array& paramArray)
    {
        for(size_t iikm = 0; iikm < paramArray.getCount(); ++iikm) {
            daeElement* pelt = searchBinding(ref,paramArray[iikm].cast());
            if( !!pelt ) {
                return pelt;
            }
        }
        return NULL;
    }

    template <typename U> static xsBoolean resolveBool(domCommon_bool_or_paramRef paddr, const U& parent) {
        if( !!paddr->getBool() ) {
            return paddr->getBool()->getValue();
        }
        if( !paddr->getParam() ) {
            ROS_WARN_STREAM("param not specified, setting to 0\n");
            return false;
        }
        for(size_t iparam = 0; iparam < parent->getNewparam_array().getCount(); ++iparam) {
            domKinematics_newparamRef pnewparam = parent->getNewparam_array()[iparam];
            if( !!pnewparam->getSid() && strcmp(pnewparam->getSid(), paddr->getParam()->getValue()) == 0 ) {
                if( !!pnewparam->getBool() ) {
                    return pnewparam->getBool()->getValue();
                }
                else if( !!pnewparam->getSIDREF() ) {
                    domKinematics_newparam::domBoolRef ptarget = daeSafeCast<domKinematics_newparam::domBool>(daeSidRef(pnewparam->getSIDREF()->getValue(), pnewparam).resolve().elt);
                    if( !ptarget ) {
                        ROS_WARN("failed to resolve %s from %s\n", pnewparam->getSIDREF()->getValue(), paddr->getID());
                        continue;
                    }
                    return ptarget->getValue();
                }
            }
        }
        ROS_WARN_STREAM(str(boost::format("failed to resolve %s\n")%paddr->getParam()->getValue()));
        return false;
    }
    template <typename U> static domFloat resolveFloat(domCommon_float_or_paramRef paddr, const U& parent) {
        if( !!paddr->getFloat() ) {
            return paddr->getFloat()->getValue();
        }
        if( !paddr->getParam() ) {
            ROS_WARN_STREAM("param not specified, setting to 0\n");
            return 0;
        }
        for(size_t iparam = 0; iparam < parent->getNewparam_array().getCount(); ++iparam) {
            domKinematics_newparamRef pnewparam = parent->getNewparam_array()[iparam];
            if( !!pnewparam->getSid() && strcmp(pnewparam->getSid(), paddr->getParam()->getValue()) == 0 ) {
                if( !!pnewparam->getFloat() ) {
                    return pnewparam->getFloat()->getValue();
                }
                else if( !!pnewparam->getSIDREF() ) {
                    domKinematics_newparam::domFloatRef ptarget = daeSafeCast<domKinematics_newparam::domFloat>(daeSidRef(pnewparam->getSIDREF()->getValue(), pnewparam).resolve().elt);
                    if( !ptarget ) {
                        ROS_WARN("failed to resolve %s from %s\n", pnewparam->getSIDREF()->getValue(), paddr->getID());
                        continue;
                    }
                    return ptarget->getValue();
                }
            }
        }
        ROS_WARN_STREAM(str(boost::format("failed to resolve %s\n")%paddr->getParam()->getValue()));
        return 0;
    }

    static bool resolveCommon_float_or_param(daeElementRef pcommon, daeElementRef parent, float& f)
    {
        daeElement* pfloat = pcommon->getChild("float");
        if( !!pfloat ) {
            std::stringstream sfloat(pfloat->getCharData());
            sfloat >> f;
            return !!sfloat;
        }
        daeElement* pparam = pcommon->getChild("param");
        if( !!pparam ) {
            if( pparam->hasAttribute("ref") ) {
                ROS_WARN_STREAM("cannot process param ref\n");
            }
            else {
                daeElement* pelt = daeSidRef(pparam->getCharData(),parent).resolve().elt;
                if( !!pelt ) {
                    ROS_WARN_STREAM(str(boost::format("found param ref: %s from %s\n")%pelt->getCharData()%pparam->getCharData()));
                }
            }
        }
        return false;
    }

    static boost::array<double,12> _matrixIdentity()
    {
        boost::array<double,12> m = {{1,0,0,0,0,1,0,0,0,0,1,0}};
        return m;
    };

    /// Gets all transformations applied to the node
    static boost::array<double,12> _getTransform(daeElementRef pelt)
    {
        boost::array<double,12> m = _matrixIdentity();
        domRotateRef protate = daeSafeCast<domRotate>(pelt);
        if( !!protate ) {
            m = _matrixFromAxisAngle(Vector3(protate->getValue()[0],protate->getValue()[1],protate->getValue()[2]), (double)(protate->getValue()[3]*(M_PI/180.0)));
            return m;
        }

        domTranslateRef ptrans = daeSafeCast<domTranslate>(pelt);
        if( !!ptrans ) {
            double scale = _GetUnitScale(pelt);
            m[3] = ptrans->getValue()[0]*scale;
            m[7] = ptrans->getValue()[1]*scale;
            m[11] = ptrans->getValue()[2]*scale;
            return m;
        }

        domMatrixRef pmat = daeSafeCast<domMatrix>(pelt);
        if( !!pmat ) {
            double scale = _GetUnitScale(pelt);
            for(int i = 0; i < 3; ++i) {
                m[4*i+0] = pmat->getValue()[4*i+0];
                m[4*i+1] = pmat->getValue()[4*i+1];
                m[4*i+2] = pmat->getValue()[4*i+2];
                m[4*i+3] = pmat->getValue()[4*i+3]*scale;
            }
            return m;
        }

        domScaleRef pscale = daeSafeCast<domScale>(pelt);
        if( !!pscale ) {
            m[0] = pscale->getValue()[0];
            m[4*1+1] = pscale->getValue()[1];
            m[4*2+2] = pscale->getValue()[2];
            return m;
        }

        domLookatRef pcamera = daeSafeCast<domLookat>(pelt);
        if( pelt->typeID() == domLookat::ID() ) {
            ROS_ERROR_STREAM("look at transform not implemented\n");
            return m;
        }

        domSkewRef pskew = daeSafeCast<domSkew>(pelt);
        if( !!pskew ) {
            ROS_ERROR_STREAM("skew transform not implemented\n");
        }

        return m;
    }

    /// Travels recursively the node parents of the given one
    /// to extract the Transform arrays that affects the node given
    template <typename T> static boost::array<double,12> _getNodeParentTransform(const T pelt) {
        domNodeRef pnode = daeSafeCast<domNode>(pelt->getParent());
        if( !pnode ) {
            return _matrixIdentity();
        }
        return _poseMult(_getNodeParentTransform(pnode), _ExtractFullTransform(pnode));
    }

    /// \brief Travel by the transformation array and calls the _getTransform method
    template <typename T> static boost::array<double,12> _ExtractFullTransform(const T pelt) {
        boost::array<double,12> t = _matrixIdentity();
        for(size_t i = 0; i < pelt->getContents().getCount(); ++i) {
            t = _poseMult(t, _getTransform(pelt->getContents()[i]));
        }
        return t;
    }

    /// \brief Travel by the transformation array and calls the _getTransform method
    template <typename T> static boost::array<double,12> _ExtractFullTransformFromChildren(const T pelt) {
        boost::array<double,12> t = _matrixIdentity();
        daeTArray<daeElementRef> children; pelt->getChildren(children);
        for(size_t i = 0; i < children.getCount(); ++i) {
            t = _poseMult(t, _getTransform(children[i]));
        }
        return t;
    }

    // decompose a matrix into a scale and rigid transform (necessary for model scales)
    void _decompose(const boost::array<double,12>& tm, Pose& tout, Vector3& vscale)
    {
        vscale.x = 1; vscale.y = 1; vscale.z = 1;
        tout = _poseFromMatrix(tm);
    }

    virtual void handleError( daeString msg )
    {
        ROS_ERROR("COLLADA error: %s\n", msg);
    }

    virtual void handleWarning( daeString msg )
    {
        ROS_WARN("COLLADA warning: %s\n", msg);
    }

    inline static double _GetUnitScale(daeElement* pelt)
    {
        return ((USERDATA*)pelt->getUserData())->scale;
    }

    domTechniqueRef _ExtractOpenRAVEProfile(const domTechnique_Array& arr)
    {
        for(size_t i = 0; i < arr.getCount(); ++i) {
            if( strcmp(arr[i]->getProfile(), "OpenRAVE") == 0 ) {
                return arr[i];
            }
        }
        return domTechniqueRef();
    }

    /// \brief returns an openrave interface type from the extra array
    boost::shared_ptr<std::string> _ExtractInterfaceType(const domExtra_Array& arr) {
        for(size_t i = 0; i < arr.getCount(); ++i) {
            if( strcmp(arr[i]->getType(),"interface_type") == 0 ) {
                domTechniqueRef tec = _ExtractOpenRAVEProfile(arr[i]->getTechnique_array());
                if( !!tec ) {
                    daeElement* ptype = tec->getChild("interface");
                    if( !!ptype ) {
                        return boost::shared_ptr<std::string>(new std::string(ptype->getCharData()));
                    }
                }
            }
        }
        return boost::shared_ptr<std::string>();
    }

    std::string _ExtractLinkName(domLinkRef pdomlink) {
        std::string linkname;
        if( !!pdomlink ) {
            if( !!pdomlink->getName() ) {
                linkname = pdomlink->getName();
            }
            if( linkname.size() == 0 && !!pdomlink->getID() ) {
                linkname = pdomlink->getID();
            }
        }
        return linkname;
    }

    bool _checkMathML(daeElementRef pelt,const std::string& type)
    {
        if( pelt->getElementName()==type ) {
            return true;
        }
        // check the substring after ':'
        std::string name = pelt->getElementName();
        std::size_t pos = name.find_last_of(':');
        if( pos == std::string::npos ) {
            return false;
        }
        return name.substr(pos+1)==type;
    }

        urdf::JointSharedPtr _getJointFromRef(xsToken targetref, daeElementRef peltref) {
        daeElement* peltjoint = daeSidRef(targetref, peltref).resolve().elt;
        domJointRef pdomjoint = daeSafeCast<domJoint> (peltjoint);

        if (!pdomjoint) {
            domInstance_jointRef pdomijoint = daeSafeCast<domInstance_joint> (peltjoint);
            if (!!pdomijoint) {
                pdomjoint = daeSafeCast<domJoint> (pdomijoint->getUrl().getElement().cast());
            }
        }

        if (!pdomjoint || pdomjoint->typeID() != domJoint::ID() || !pdomjoint->getName()) {
            ROS_WARN_STREAM(str(boost::format("could not find collada joint %s!\n")%targetref));
            return urdf::JointSharedPtr();
        }

        urdf::JointSharedPtr pjoint;
        std::string name(pdomjoint->getName());
        if (_model->joints_.find(name) == _model->joints_.end()) {
            pjoint.reset();
        } else {
            pjoint = _model->joints_.find(name)->second;
        }
        if(!pjoint) {
            ROS_WARN_STREAM(str(boost::format("could not find openrave joint %s!\n")%pdomjoint->getName()));
        }
        return pjoint;
    }

    /// \brief go through all kinematics binds to get a kinematics/visual pair
    /// \param kiscene instance of one kinematics scene, binds the kinematic and visual models
    /// \param bindings the extracted bindings
    static void _ExtractKinematicsVisualBindings(domInstance_with_extraRef viscene, domInstance_kinematics_sceneRef kiscene, KinematicsSceneBindings& bindings)
    {
        domKinematics_sceneRef kscene = daeSafeCast<domKinematics_scene> (kiscene->getUrl().getElement().cast());
        if (!kscene) {
            return;
        }
        for (size_t imodel = 0; imodel < kiscene->getBind_kinematics_model_array().getCount(); imodel++) {
            domArticulated_systemRef articulated_system; // if filled, contains robot-specific information, so create a robot
            domBind_kinematics_modelRef kbindmodel = kiscene->getBind_kinematics_model_array()[imodel];
            if (!kbindmodel->getNode()) {
                ROS_WARN_STREAM("do not support kinematics models without references to nodes\n");
                continue;
            }

            // visual information
            domNodeRef node = daeSafeCast<domNode>(daeSidRef(kbindmodel->getNode(), viscene->getUrl().getElement()).resolve().elt);
            if (!node) {
                ROS_WARN_STREAM(str(boost::format("bind_kinematics_model does not reference valid node %s\n")%kbindmodel->getNode()));
                continue;
            }

            //  kinematics information
            daeElement* pelt = searchBinding(kbindmodel,kscene);
            domInstance_kinematics_modelRef kimodel = daeSafeCast<domInstance_kinematics_model>(pelt);
            if (!kimodel) {
                if( !pelt ) {
                    ROS_WARN_STREAM("bind_kinematics_model does not reference element\n");
                }
                else {
                    ROS_WARN_STREAM(str(boost::format("bind_kinematics_model cannot find reference to %s:\n")%pelt->getElementName()));
                }
                continue;
            }
            bindings.listKinematicsVisualBindings.push_back(std::make_pair(node,kimodel));
        }
        // axis info
        for (size_t ijoint = 0; ijoint < kiscene->getBind_joint_axis_array().getCount(); ++ijoint) {
            domBind_joint_axisRef bindjoint = kiscene->getBind_joint_axis_array()[ijoint];
            daeElementRef pjtarget = daeSidRef(bindjoint->getTarget(), viscene->getUrl().getElement()).resolve().elt;
            if (!pjtarget) {
                ROS_ERROR_STREAM(str(boost::format("Target Node %s NOT found!!!\n")%bindjoint->getTarget()));
                continue;
            }
            daeElement* pelt = searchBinding(bindjoint->getAxis(),kscene);
            domAxis_constraintRef pjointaxis = daeSafeCast<domAxis_constraint>(pelt);
            if (!pjointaxis) {
                continue;
            }
            bindings.listAxisBindings.push_back(JointAxisBinding(pjtarget, pjointaxis, bindjoint->getValue(), NULL, NULL));
        }
    }

    static void _ExtractPhysicsBindings(domCOLLADA::domSceneRef allscene, KinematicsSceneBindings& bindings)
    {
        for(size_t iphysics = 0; iphysics < allscene->getInstance_physics_scene_array().getCount(); ++iphysics) {
            domPhysics_sceneRef pscene = daeSafeCast<domPhysics_scene>(allscene->getInstance_physics_scene_array()[iphysics]->getUrl().getElement().cast());
            for(size_t imodel = 0; imodel < pscene->getInstance_physics_model_array().getCount(); ++imodel) {
                domInstance_physics_modelRef ipmodel = pscene->getInstance_physics_model_array()[imodel];
                domPhysics_modelRef pmodel = daeSafeCast<domPhysics_model> (ipmodel->getUrl().getElement().cast());
                domNodeRef nodephysicsoffset = daeSafeCast<domNode>(ipmodel->getParent().getElement().cast());
                for(size_t ibody = 0; ibody < ipmodel->getInstance_rigid_body_array().getCount(); ++ibody) {
                    LinkBinding lb;
                    lb.irigidbody = ipmodel->getInstance_rigid_body_array()[ibody];
                    lb.node = daeSafeCast<domNode>(lb.irigidbody->getTarget().getElement().cast());
                    lb.rigidbody = daeSafeCast<domRigid_body>(daeSidRef(lb.irigidbody->getBody(),pmodel).resolve().elt);
                    lb.nodephysicsoffset = nodephysicsoffset;
                    if( !!lb.rigidbody && !!lb.node ) {
                        bindings.listLinkBindings.push_back(lb);
                    }
                }
            }
        }
    }


    size_t _countChildren(daeElement* pelt) {
        size_t c = 1;
        daeTArray<daeElementRef> children;
        pelt->getChildren(children);
        for (size_t i = 0; i < children.getCount(); ++i) {
            c += _countChildren(children[i]);
        }
        return c;
    }

    void _processUserData(daeElement* pelt, double scale)
    {
        // getChild could be optimized since asset tag is supposed to appear as the first element
        domAssetRef passet = daeSafeCast<domAsset> (pelt->getChild("asset"));
        if (!!passet && !!passet->getUnit()) {
            scale = passet->getUnit()->getMeter();
        }

        _vuserdata.push_back(USERDATA(scale));
        pelt->setUserData(&_vuserdata.back());
        daeTArray<daeElementRef> children;
        pelt->getChildren(children);
        for (size_t i = 0; i < children.getCount(); ++i) {
            if (children[i] != passet) {
                _processUserData(children[i], scale);
            }
        }
    }

    USERDATA* _getUserData(daeElement* pelt)
    {
        BOOST_ASSERT(!!pelt);
        void* p = pelt->getUserData();
        BOOST_ASSERT(!!p);
        return (USERDATA*)p;
    }

    //
    // openrave math functions (from geometry.h)
    //

    static Vector3 _poseMult(const Pose& p, const Vector3& v)
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
        Vector3 vnew;
        vnew.x = (1-xx-yy) * v.x + (wx-yz) * v.y + (wy+xz)*v.z + p.position.x;
        vnew.y = (wx+yz) * v.x + (1-ww-yy) * v.y + (xy-wz)*v.z + p.position.y;
        vnew.z = (wy-xz) * v.x + (xy+wz) * v.y + (1-ww-xx)*v.z + p.position.z;
        return vnew;
    }

    static Vector3 _poseMult(const boost::array<double,12>& m, const Vector3& v)
    {
        Vector3 vnew;
        vnew.x = m[4*0+0] * v.x + m[4*0+1] * v.y + m[4*0+2] * v.z + m[4*0+3];
        vnew.y = m[4*1+0] * v.x + m[4*1+1] * v.y + m[4*1+2] * v.z + m[4*1+3];
        vnew.z = m[4*2+0] * v.x + m[4*2+1] * v.y + m[4*2+2] * v.z + m[4*2+3];
        return vnew;
    }

    static boost::array<double,12> _poseMult(const boost::array<double,12>& m0, const boost::array<double,12>& m1)
    {
        boost::array<double,12> mres;
        mres[0*4+0] = m0[0*4+0]*m1[0*4+0]+m0[0*4+1]*m1[1*4+0]+m0[0*4+2]*m1[2*4+0];
        mres[0*4+1] = m0[0*4+0]*m1[0*4+1]+m0[0*4+1]*m1[1*4+1]+m0[0*4+2]*m1[2*4+1];
        mres[0*4+2] = m0[0*4+0]*m1[0*4+2]+m0[0*4+1]*m1[1*4+2]+m0[0*4+2]*m1[2*4+2];
        mres[1*4+0] = m0[1*4+0]*m1[0*4+0]+m0[1*4+1]*m1[1*4+0]+m0[1*4+2]*m1[2*4+0];
        mres[1*4+1] = m0[1*4+0]*m1[0*4+1]+m0[1*4+1]*m1[1*4+1]+m0[1*4+2]*m1[2*4+1];
        mres[1*4+2] = m0[1*4+0]*m1[0*4+2]+m0[1*4+1]*m1[1*4+2]+m0[1*4+2]*m1[2*4+2];
        mres[2*4+0] = m0[2*4+0]*m1[0*4+0]+m0[2*4+1]*m1[1*4+0]+m0[2*4+2]*m1[2*4+0];
        mres[2*4+1] = m0[2*4+0]*m1[0*4+1]+m0[2*4+1]*m1[1*4+1]+m0[2*4+2]*m1[2*4+1];
        mres[2*4+2] = m0[2*4+0]*m1[0*4+2]+m0[2*4+1]*m1[1*4+2]+m0[2*4+2]*m1[2*4+2];
        mres[3] = m1[3] * m0[0] + m1[7] * m0[1] + m1[11] * m0[2] + m0[3];
        mres[7] = m1[3] * m0[4] + m1[7] * m0[5] + m1[11] * m0[6] + m0[7];
        mres[11] = m1[3] * m0[8] + m1[7] * m0[9] + m1[11] * m0[10] + m0[11];
        return mres;
    }

    static Pose _poseMult(const Pose& p0, const Pose& p1)
    {
        Pose p;
        p.position = _poseMult(p0,p1.position);
        p.rotation = _quatMult(p0.rotation,p1.rotation);
        return p;
    }

    static Pose _poseInverse(const Pose& p)
    {
        Pose pinv;
        pinv.rotation.x = -p.rotation.x;
        pinv.rotation.y = -p.rotation.y;
        pinv.rotation.z = -p.rotation.z;
        pinv.rotation.w = p.rotation.w;
        Vector3 t = _poseMult(pinv,p.position);
        pinv.position.x = -t.x;
        pinv.position.y = -t.y;
        pinv.position.z = -t.z;
        return pinv;
    }

    static Rotation _quatMult(const Rotation& quat0, const Rotation& quat1)
    {
        Rotation q;
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

    static boost::array<double,12> _matrixFromAxisAngle(const Vector3& axis, double angle)
    {
        return _matrixFromQuat(_quatFromAxisAngle(axis.x,axis.y,axis.z,angle));
    }

    static boost::array<double,12> _matrixFromQuat(const Rotation& quat)
    {
        boost::array<double,12> m;
        double qq1 = 2*quat.x*quat.x;
        double qq2 = 2*quat.y*quat.y;
        double qq3 = 2*quat.z*quat.z;
        m[4*0+0] = 1 - qq2 - qq3;
        m[4*0+1] = 2*(quat.x*quat.y - quat.w*quat.z);
        m[4*0+2] = 2*(quat.x*quat.z + quat.w*quat.y);
        m[4*0+3] = 0;
        m[4*1+0] = 2*(quat.x*quat.y + quat.w*quat.z);
        m[4*1+1] = 1 - qq1 - qq3;
        m[4*1+2] = 2*(quat.y*quat.z - quat.w*quat.x);
        m[4*1+3] = 0;
        m[4*2+0] = 2*(quat.x*quat.z - quat.w*quat.y);
        m[4*2+1] = 2*(quat.y*quat.z + quat.w*quat.x);
        m[4*2+2] = 1 - qq1 - qq2;
        m[4*2+3] = 0;
        return m;
    }

    static Pose _poseFromMatrix(const boost::array<double,12>& m)
    {
        Pose t;
        t.rotation = _quatFromMatrix(m);
        t.position.x = m[3];
        t.position.y = m[7];
        t.position.z = m[11];
        return t;
    }

    static boost::array<double,12> _matrixFromPose(const Pose& t)
    {
        boost::array<double,12> m = _matrixFromQuat(t.rotation);
        m[3] = t.position.x;
        m[7] = t.position.y;
        m[11] = t.position.z;
        return m;
    }

    static Rotation _quatFromAxisAngle(double x, double y, double z, double angle)
    {
        Rotation q;
        double axislen = std::sqrt(x*x+y*y+z*z);
        if( axislen == 0 ) {
            return q;
        }
        angle *= 0.5;
        double sang = std::sin(angle)/axislen;
        q.w = std::cos(angle);
        q.x = x*sang;
        q.y = y*sang;
        q.z = z*sang;
        return q;
    }

    static Rotation _quatFromMatrix(const boost::array<double,12>& mat)
    {
        Rotation rot;
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

    static double _dot3(const Vector3& v0, const Vector3& v1)
    {
        return v0.x*v1.x + v0.y*v1.y + v0.z*v1.z;
    }
    static Vector3 _cross3(const Vector3& v0, const Vector3& v1)
    {
        Vector3 v;
        v.x = v0.y * v1.z - v0.z * v1.y;
        v.y = v0.z * v1.x - v0.x * v1.z;
        v.z = v0.x * v1.y - v0.y * v1.x;
        return v;
    }
    static Vector3 _sub3(const Vector3& v0, const Vector3& v1)
    {
        Vector3 v;
        v.x = v0.x-v1.x;
        v.y = v0.y-v1.y;
        v.z = v0.z-v1.z;
        return v;
    }
    static Vector3 _add3(const Vector3& v0, const Vector3& v1)
    {
        Vector3 v;
        v.x = v0.x+v1.x;
        v.y = v0.y+v1.y;
        v.z = v0.z+v1.z;
        return v;
    }
    static Vector3 _normalize3(const Vector3& v0)
    {
        Vector3 v;
        double norm = std::sqrt(v0.x*v0.x+v0.y*v0.y+v0.z*v0.z);
        v.x = v0.x/norm;
        v.y = v0.y/norm;
        v.z = v0.z/norm;
        return v;
    }

    boost::shared_ptr<DAE> _collada;
    domCOLLADA* _dom;
    std::vector<USERDATA> _vuserdata; // all userdata
    int _nGlobalSensorId, _nGlobalManipulatorId;
    std::string _filename;
    std::string _resourcedir;
    urdf::ModelInterfaceSharedPtr _model;
    Pose _RootOrigin;
    Pose _VisualRootOrigin;
};




urdf::ModelInterfaceSharedPtr parseCollada(const std::string &xml_str)
{
    urdf::ModelInterfaceSharedPtr model(new ModelInterface);

    ColladaModelReader reader(model);
    if (!reader.InitFromData(xml_str))
        model.reset();
    return model;
}
}
