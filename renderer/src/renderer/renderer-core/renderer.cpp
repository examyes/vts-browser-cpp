#include <unordered_map>

#include <boost/optional/optional_io.hpp>
#include <vts-libs/vts/urltemplate.hpp>
#include <vts-libs/vts/nodeinfo.hpp>
#include <dbglog/dbglog.hpp>
#include <utility/uri.hpp>

#include <renderer/gpuResources.h>
#include <renderer/gpuContext.h>
#include <renderer/statistics.h>

#include "map.h"
#include "mapConfig.h"
#include "renderer.h"
#include "resourceManager.h"
#include "mapResources.h"
#include "csConvertor.h"
#include "resource.h"

namespace melown
{

using namespace vtslibs::vts;
using namespace vtslibs::registry;

namespace {

const vec3 lowerUpperCombine(uint32 i)
{
    vec3 res;
    res(0) = (i >> 0) % 2;
    res(1) = (i >> 1) % 2;
    res(2) = (i >> 2) % 2;
    return res;
}

const vec4 column(const mat4 &m, uint32 index)
{
    return vec4(m(index, 0), m(index, 1), m(index, 2), m(index, 3));
}

}

class RendererImpl : public Renderer
{
public:
    class SurfaceInfo : public SurfaceConfig
    {
    public:
        SurfaceInfo(const SurfaceConfig &surface, RendererImpl *renderer)
            : SurfaceConfig(surface)
        {
            urlMeta.parse(renderer->convertPath(urls3d->meta,
                                      renderer->mapConfig->name));
            urlMesh.parse(renderer->convertPath(urls3d->mesh,
                                      renderer->mapConfig->name));
            urlIntTex.parse(renderer->convertPath(urls3d->texture,
                                        renderer->mapConfig->name));
        }

        UrlTemplate urlMeta;
        UrlTemplate urlMesh;
        UrlTemplate urlIntTex;
    };

    class BoundInfo : public BoundLayer
    {
    public:
        BoundInfo(const BoundLayer &layer, RendererImpl *impl)
            : BoundLayer(layer), impl(impl)
        {
            urlExtTex.parse(url);
            if (metaUrl)
            {
                urlMeta.parse(*metaUrl);
                urlMask.parse(*maskUrl);
            }
        }

        RendererImpl *impl;
        UrlTemplate urlExtTex;
        UrlTemplate urlMeta;
        UrlTemplate urlMask;
    };

    class BoundParamInfo : public View::BoundLayerParams
    {
    public:
        typedef std::vector<BoundParamInfo> list;

        BoundParamInfo(const View::BoundLayerParams &params)
            : View::BoundLayerParams(params), bound(nullptr),
              watertight(true), availCache(false),
              vars(0), orig(0)
        {}
        
        bool available(bool primaryLod = false)
        {
            if (availCache)
                return true;  
            
            // check mask texture
            if (bound->metaUrl && !watertight)
            {
                if (primaryLod)
                {
                    GpuTexture *t = impl->map->resources->getTexture(
                                bound->urlMask(vars));
                    if (!*t)
                        return false;
                }
                else
                {
                    if (!impl->map->resources->ready(
                                bound->urlMask(vars)))
                        return false;
                }
            }
            
            // check color texture
            if (primaryLod)
            {
                GpuTexture *t = impl->map->resources->getTexture(
                            bound->urlExtTex(vars));
                t->impl->availTest = bound->availability.get_ptr();
                return availCache = *t;
            }
            else
                return availCache = impl->map->resources->ready(
                            bound->urlExtTex(vars));
        }
        
        void varsFallDown(int depth)
        {
            if (depth == 0)
                return;
            vars.tileId.lod -= depth;
            vars.tileId.x >>= depth;
            vars.tileId.y >>= depth;
            vars.localId.lod -= depth;
            vars.localId.x >>= depth;
            vars.localId.y >>= depth;
        }
        
        const mat3f uvMatrix() const
        {
            int dep = depth();
            if (dep == 0)
                return upperLeftSubMatrix(identityMatrix()).cast<float>();
            double scale = 1.0 / (1 << dep);
            double tx = scale * (orig.localId.x
                                 - ((orig.localId.x >> dep) << dep));
            double ty = scale * (orig.localId.y
                                 - ((orig.localId.y >> dep) << dep));
            ty = 1 - scale - ty;
            mat3f m;
            m << scale, 0, tx,
                 0, scale, ty,
                 0, 0, 1;
            return m;
        }
        
        bool canGoUp() const
        {
            return vars.localId.lod <= bound->lodRange.min + 1;
        }
        
        int depth() const
        {
            return std::max(nodeInfo->nodeId().lod - bound->lodRange.max, 0)
                    + orig.localId.lod - vars.localId.lod;
        }

        void prepare(const NodeInfo &nodeInfo, RendererImpl *impl,
                     uint32 subMeshIndex)
        {
            this->nodeInfo = &nodeInfo;
            this->impl = impl;
            bound = impl->getBoundInfo(id);
            if (!bound)
                return;
            orig = vars = UrlTemplate::Vars(nodeInfo.nodeId(), local(nodeInfo),
                                            subMeshIndex);
            varsFallDown(depth());
            
            if (bound->metaUrl)
            { // bound meta nodes
                UrlTemplate::Vars v(vars);
                v.tileId.x &= ~255;
                v.tileId.y &= ~255;
                v.localId.x &= ~255;
                v.localId.y &= ~255;
                BoundMetaTile *bmt = impl->map->resources->getBoundMetaTile(
                            bound->urlMeta(v));
                if (!*bmt)
                {
                    bound = nullptr;
                    return;
                }
                
                uint8 f = bmt->flags[(vars.tileId.y & 255) * 256
                        + (vars.tileId.x & 255)];
                if ((f & BoundLayer::MetaFlags::available)
                        != BoundLayer::MetaFlags::available)
                {
                    bound = nullptr;
                    return;
                }
                
                watertight = (f & BoundLayer::MetaFlags::watertight)
                        == BoundLayer::MetaFlags::watertight;
            }
        }

        bool invalid() const
        {
            if (!bound)
                return true;
            TileId t = nodeInfo->nodeId();
            int m = bound->lodRange.min;
            if (t.lod < m)
                return true;
            t.x >>= t.lod - m;
            t.y >>= t.lod - m;
            if (t.x < bound->tileRange.ll[0] || t.x > bound->tileRange.ur[0])
                return true;
            if (t.y < bound->tileRange.ll[1] || t.y > bound->tileRange.ur[1])
                return true;
            return false;
        }

        bool operator < (const BoundParamInfo &rhs) const
        {
            return depth() > rhs.depth();
        }
        
        UrlTemplate::Vars orig;
        UrlTemplate::Vars vars;
        const NodeInfo *nodeInfo;
        RendererImpl *impl;
        BoundInfo *bound;
        bool watertight;
        
    private:
        bool availCache;
    };

    RendererImpl(MapImpl *map) : map(map), mapConfig(nullptr),
        shader(nullptr), gpuContext(nullptr)
    {}

    void renderInitialize(GpuContext *gpuContext) override
    {
        this->gpuContext = gpuContext;
    }

    void reorderBoundLayer(const NodeInfo &nodeInfo, uint32 subMeshIndex,
            BoundParamInfo::list &boundList)
    {
        for (BoundParamInfo &b : boundList)
            b.prepare(nodeInfo, this, subMeshIndex);
        
        // erase invalid layers
        boundList.erase(std::remove_if(boundList.begin(), boundList.end(),
            [](BoundParamInfo &a){ return a.invalid(); }),
                        boundList.end());
        if (boundList.empty())
            return;
            
        // sort by depth and priority
        std::stable_sort(boundList.begin(), boundList.end());
        std::reverse(boundList.begin(), boundList.end()); // render in reverse
        
        // skip overlapping layers
        auto it = boundList.begin(), et = boundList.end();
        while (it != et && !it->watertight)
            it++;
        if (it != et)
            boundList.erase(++it, et);
        
        // show lower resolution when appropriate
        for (BoundParamInfo &b : boundList)
        {
            for (uint32  i = 0; i < 5; i++)
            {
                if (b.available(i == 0) || !b.canGoUp())
                    break;
                b.varsFallDown(1);
            }
        }
        
        // erase not-ready layers
        boundList.erase(std::remove_if(boundList.begin(), boundList.end(),
            [](BoundParamInfo &a){ return !a.available(); }),
                        boundList.end());
    }

    void drawBox(const mat4f &mvp)
    {
        GpuMeshRenderable *mesh
                = map->resources->getMeshRenderable("data/cube.obj");
        if (!*mesh)
            return;

        shaderColor->bind();
        shaderColor->uniformMat4(0, mvp.data());
        shaderColor->uniformVec3(8, vec3f(1, 0, 0).data());
        gpuContext->wiremode(true);
        mesh->draw();
        gpuContext->wiremode(false);
        shader->bind();
    }

    bool renderNode(const NodeInfo &nodeInfo,
                    const SurfaceInfo &surface,
                    const View::BoundLayerParams::list &boundList,
                    bool onlyCheckReady)
    {
        // nodeId
        const TileId nodeId = nodeInfo.nodeId();

        // statistics
        if (!onlyCheckReady)
        {
            map->statistics->meshesRenderedTotal++;
            map->statistics->meshesRenderedPerLod[
                    std::min<uint32>(nodeId.lod, MapStatistics::MaxLods-1)]++;
        }

        // aggregate mesh
        MeshAggregate *meshAgg = map->resources->getMeshAggregate
                (surface.urlMesh(UrlTemplate::Vars(nodeId, local(nodeInfo))));
        if (!*meshAgg)
            return false;

        // iterate over all submeshes
        for (uint32 subMeshIndex = 0, e = meshAgg->submeshes.size();
             subMeshIndex != e; subMeshIndex++)
        {
            const MeshPart &part = meshAgg->submeshes[subMeshIndex];
            GpuMeshRenderable *mesh = part.renderable.get();
            if (!onlyCheckReady)
            {
                mat4f mvp = (viewProj * part.normToPhys).cast<float>();
                shader->uniformMat4(0, mvp.data());
                //drawBox(mvp);
            }
            
            // internal texture
            if (part.internalUv)
            {
                UrlTemplate::Vars vars(nodeId, local(nodeInfo), subMeshIndex);
                GpuTexture *texture = map->resources->getTexture(
                            surface.urlIntTex(vars));
                if (onlyCheckReady)
                {
                    if (!*texture)
                        return false;
                }
                else
                {
                    if (!*texture)
                        continue;
                    texture->bind();
                    mat3f uvm = upperLeftSubMatrix(identityMatrix())
                            .cast<float>();
                    shader->uniformMat3(4, uvm.data());
                    shader->uniform(8, 0); // internal uv
                    mesh->draw();
                }
                continue;
            }

            // external bound textures
            if (part.externalUv)
            {
                BoundParamInfo::list bls(boundList.begin(), boundList.end());
                if (part.textureLayer)
                    bls.push_back(BoundParamInfo(View::BoundLayerParams(
                        mapConfig->boundLayers.get(part.textureLayer).id)));
                reorderBoundLayer(nodeInfo, subMeshIndex, bls);
                if (onlyCheckReady && bls.empty())
                    return false;
                if (!onlyCheckReady)
                for (BoundParamInfo &b : bls)
                {
                    if (!b.watertight)
                    {
                        gpuContext->activeTextureUnit(1);
                        map->resources->getTexture(b.bound->urlMask(b.vars))
                                ->bind();
                        gpuContext->activeTextureUnit(0);
                        shader->uniform(9, 1); // use mask
                    }
                    map->resources->getTexture(b.bound->urlExtTex(b.vars))
                            ->bind();
                    mat3f uvm = b.uvMatrix();
                    shader->uniformMat3(4, uvm.data());
                    shader->uniform(8, 1); // external uv
                    mesh->draw();
                    if (!b.watertight)
                        shader->uniform(9, 0); // do not use mask
                }
            }
        }
        
        return true;
    }

    const std::string &pickSurface(
        const View::BoundLayerParams::list *&boundList)
    {
        // todo - pick first surface for now
        auto it = mapConfig->view.surfaces.begin();
        boundList = &it->second;
        return it->first;
    }

    const bool visibilityTest(const NodeInfo &nodeInfo, const MetaNode &node)
    {
        vec4 c0 = column(viewProj, 0);
        vec4 c1 = column(viewProj, 1);
        vec4 c2 = column(viewProj, 2);
        vec4 c3 = column(viewProj, 3);
        vec4 planes[6] = {
            c3 + c0, c3 - c0,
            c3 + c1, c3 - c1,
            c3 + c2, c3 - c2,
        };
        vec3 fl = vecFromUblas<vec3>(node.extents.ll);
        vec3 fu = vecFromUblas<vec3>(node.extents.ur);
        vec3 el = vecFromUblas<vec3>
          (mapConfig->referenceFrame.division.extents.ll);
        vec3 eu = vecFromUblas<vec3>
          (mapConfig->referenceFrame.division.extents.ur);
        vec3 box[2] = {
            fl.cwiseProduct(eu - el) + el,
            fu.cwiseProduct(eu - el) + el,
        };
        for (uint32 i = 0; i < 6; i++)
        {
            vec4 &p = planes[i]; // current plane
            vec3 pv = vec3( // current p-vertex
                box[!!(p[0] > 0)](0),
                box[!!(p[1] > 0)](1),
                box[!!(p[2] > 0)](2)
            );
            double d = dot(vec4to3(p), pv);
            if (d < -p[3])
                return false;
        }
        return true;
    }

    const bool coarsenessTest(const NodeInfo &nodeInfo, const MetaNode &node)
    {
        if (!node.applyTexelSize() && !node.applyDisplaySize())
            return false;
        bool result = true;
        if (node.applyTexelSize())
        {
            vec3 fl = vecFromUblas<vec3>(node.extents.ll);
            vec3 fu = vecFromUblas<vec3>(node.extents.ur);
            vec3 el = vecFromUblas<vec3>
                    (mapConfig->referenceFrame.division.extents.ll);
            vec3 eu = vecFromUblas<vec3>
                    (mapConfig->referenceFrame.division.extents.ur);
            vec3 up = perpendicularUnitVector * node.texelSize;
            for (uint32 i = 0; i < 8; i++)
            {
                vec3 f = lowerUpperCombine(i).cwiseProduct(fu - fl) + fl;
                vec3 c1 = f.cwiseProduct(eu - el) + el - up * 0.5;
                vec3 c2 = c1 + up;
                c1 = vec4to3(viewProj * vec3to4(c1, 1), true);
                c2 = vec4to3(viewProj * vec3to4(c2, 1), true);
                double len = length(vec3(c2 - c1)) * windowHeight;
                result = result && len < 1.2;
            }
        }
        if (node.applyDisplaySize())
        {
            result = false;
        }
        return result;
    }

    void traverse(const NodeInfo &nodeInfo)
    {
        // nodeId
        const TileId nodeId = nodeInfo.nodeId();

        // statistics
        map->statistics->metaNodesTraversedTotal++;
        map->statistics->metaNodesTraversedPerLod[
                std::min<uint32>(nodeId.lod, MapStatistics::MaxLods-1)]++;

        // pick surface
        const View::BoundLayerParams::list *boundList = nullptr;
        const SurfaceInfo *surface = getSurfaceInfo(pickSurface(boundList));
        if (!surface)
            return;

        // pick meta tile
        const TileId tileId(nodeId.lod,
                            (nodeId.x >> binaryOrder) << binaryOrder,
                            (nodeId.y >> binaryOrder) << binaryOrder);
        const MetaTile *tile = map->resources->getMetaTile
                (surface->urlMeta(UrlTemplate::Vars(tileId)));
        if (!*tile)
            return;

        // pick meta node
        const MetaNode *node = tile->get(nodeId, std::nothrow_t());
        if (!node)
            return;

        if (!visibilityTest(nodeInfo, *node))
            return;
        
        if (node->geometry() && coarsenessTest(nodeInfo, *node))
        {
            // deep enough, render and return
            renderNode(nodeInfo, *surface, *boundList, false);
            return;
        }
        
        // check children
        Children childs = children(nodeId);
        bool allChildsReady = true;
        if (node->geometry())
        {
            for (uint32 i = 0; i < childs.size(); i++)
            {
                if (node->flags() & (MetaNode::Flag::ulChild << i))
                    allChildsReady = allChildsReady
                        && renderNode(nodeInfo.child(childs[i]),
                                      *surface, *boundList, true);
            }
        }
        
        // traverse children
        if (allChildsReady)
        {
            for (uint32 i = 0; i < childs.size(); i++)
            {
                if (node->flags() & (MetaNode::Flag::ulChild << i))
                    traverse(nodeInfo.child(childs[i]));
            }
            return;
        }
        
        renderNode(nodeInfo, *surface, *boundList, false);
    }

    void updateCamera()
    {
        // todo position conversions
        if (mapConfig->position.type
                != vtslibs::registry::Position::Type::objective)
            throw std::invalid_argument("unsupported position type");
        if (mapConfig->position.heightMode
                != vtslibs::registry::Position::HeightMode::fixed)
            throw std::invalid_argument("unsupported position height mode");

        vec3 center = vecFromUblas<vec3>(mapConfig->position.position);
        vec3 rot = vecFromUblas<vec3>(mapConfig->position.orientation);

        vec3 dir(1, 0, 0);
        vec3 up(0, 0, -1);

        { // apply rotation
            mat3 tmp = upperLeftSubMatrix(rotationMatrix(2, degToRad(-rot(0))))
                    * upperLeftSubMatrix(rotationMatrix(1, degToRad(-rot(1))));
            dir = tmp * dir;
            up = tmp * up;
        }

        switch (mapConfig->srs.get
                (mapConfig->referenceFrame.model.navigationSrs).type)
        {
        case vtslibs::registry::Srs::Type::projected:
        {
            // swap XY
            std::swap(dir(0), dir(1));
            std::swap(up(0), up(1));
            // invert Z
            dir(2) *= -1;
            up(2) *= -1;
            // add center of orbit (transform to navigation srs)
            dir += center;
            up += center;
            // transform to physical srs
            center = map->convertor->navToPhys(center);
            dir = map->convertor->navToPhys(dir);
            up = map->convertor->navToPhys(up);
            // points -> vectors
            dir = normalize(dir - center);
            up = normalize(up - center);
        } break;
        case vtslibs::registry::Srs::Type::geographic:
        {
            // find lat-lon coordinates of points moved to north and east
            vec3 n2 = map->convertor->navGeodesicDirect(center, 0, 100);
            vec3 e2 = map->convertor->navGeodesicDirect(center, 90, 100);
            // transform to physical srs
            center = map->convertor->navToPhys(center);
            vec3 n = map->convertor->navToPhys(n2);
            vec3 e = map->convertor->navToPhys(e2);
            // points -> vectors
            n = normalize(n - center);
            e = normalize(e - center);
            // construct NED coordinate system
            vec3 d = normalize(cross(n, e));
            e = normalize(cross(n, d));
            mat3 tmp = (mat3() << n, e, d).finished();
            // rotate original vectors
            dir = tmp * dir;
            up = tmp * up;
            dir = normalize(dir);
            up = normalize(up);
        } break;
        default:
            throw std::invalid_argument("not implemented navigation srs type");
        }

        double dist = mapConfig->position.verticalExtent
                * 0.5 / tan(degToRad(mapConfig->position.verticalFov * 0.5));
        mat4 view = lookAt(center - dir * dist, center, up);
        mat4 proj = perspectiveMatrix(mapConfig->position.verticalFov,
                        (double)windowWidth / (double)windowHeight,
                        dist * 0.1, dist * 10);
        viewProj = proj * view;
        perpendicularUnitVector = normalize(cross(up, dir));
    }

    const std::string convertPath(const std::string &path,
                                  const std::string &parent)
    {
        return utility::Uri(parent).resolve(path).str();
    }

    const bool configLoaded()
    {
        mapConfig = map->resources->getMapConfig(map->mapConfigPath);
        if (!mapConfig || !*mapConfig)
            return false;

        bool ok = true;
        for (auto &&bl : mapConfig->boundLayers)
        {
            if (!bl.external())
                continue;
            std::string url = convertPath(bl.url, mapConfig->name);
            ExternalBoundLayer *r
                    = map->resources->getExternalBoundLayer(url);
            if (!*r)
                ok = false;
            else
            {
                r->id = bl.id;
                r->url = convertPath(r->url, url);
                if (r->metaUrl)
                    r->metaUrl = convertPath(*r->metaUrl, url);
                if (r->maskUrl)
                    r->maskUrl = convertPath(*r->maskUrl, url);
                if (r->creditsUrl)
                    r->creditsUrl = convertPath(*r->creditsUrl, url);
                mapConfig->boundLayers.replace(*r);
            }
        }
        return ok;
    }

    void onceInitialize()
    {
        map->convertor = std::shared_ptr<CsConvertor>(CsConvertor::create(
            mapConfig->referenceFrame.model.physicalSrs,
            mapConfig->referenceFrame.model.navigationSrs,
            mapConfig->referenceFrame.model.publicSrs,
            *mapConfig
        ));

        binaryOrder = mapConfig->referenceFrame.metaBinaryOrder;

        for (auto &&bl : mapConfig->boundLayers)
        {
            boundInfos[bl.id] = std::shared_ptr<BoundInfo>
                    (new BoundInfo(bl, this));
        }

        for (auto &&sr : mapConfig->surfaces)
        {
            surfaceInfos[sr.id] = std::shared_ptr<SurfaceInfo>
                    (new SurfaceInfo(sr, this));
        }

        // todo - temporarily reset camera
        mapConfig->position.orientation = math::Point3(0, -90, 0);

        LOG(info3) << "position: " << mapConfig->position.position;
        LOG(info3) << "rotation: " << mapConfig->position.orientation;

        /*
        for (auto &&bl : mapConfig->boundLayers)
        {
            LOG(info3) << bl.type;
            LOG(info3) << bl.id;
            LOG(info3) << bl.url;
            LOG(info3) << bl.metaUrl;
            LOG(info3) << bl.maskUrl;
            LOG(info3) << bl.creditsUrl;
        }
        */
    }

    void renderTick(uint32 windowWidth, uint32 windowHeight) override
    {
        if (!configLoaded())
            return;

        this->windowWidth = windowWidth;
        this->windowHeight = windowHeight;

        shaderColor = map->resources->getShader("data/shaders/color.glsl");
        if (!*shaderColor)
            return;

        shader = map->resources->getShader("data/shaders/a.glsl");
        if (!*shader)
            return;
        shader->bind();

        if (!map->convertor)
            onceInitialize();

        updateCamera();

        NodeInfo nodeInfo(mapConfig->referenceFrame, TileId(),
                          false, *mapConfig);
        traverse(nodeInfo);

        map->statistics->frameIndex++;
    }

    void renderFinalize() override
    {}

    SurfaceInfo *getSurfaceInfo(const std::string &id)
    {
        auto it = surfaceInfos.find(id);
        if (it == surfaceInfos.end())
            return nullptr;
        return it->second.get();
    }

    BoundInfo *getBoundInfo(const std::string &id)
    {
        auto it = boundInfos.find(id);
        if (it == boundInfos.end())
            return nullptr;
        return it->second.get();
    }

    std::unordered_map<std::string, std::shared_ptr<SurfaceInfo>> surfaceInfos;
    std::unordered_map<std::string, std::shared_ptr<BoundInfo>> boundInfos;

    vec3 perpendicularUnitVector;
    mat4 viewProj;
    uint32 windowWidth;
    uint32 windowHeight;
    MapImpl *map;
    MapConfig *mapConfig;
    GpuShader *shader;
    GpuShader *shaderColor;
    GpuContext *gpuContext;
    uint32 binaryOrder;
};

Renderer::Renderer()
{}

Renderer::~Renderer()
{}

Renderer *Renderer::create(MapImpl *map)
{
    return new RendererImpl(map);
}

} // namespace melown
