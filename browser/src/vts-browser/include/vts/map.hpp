#ifndef MAP_H_jihsefk
#define MAP_H_jihsefk

#include <string>
#include <memory>
#include <vector>

#include "foundation.hpp"
#include "point.hpp"

namespace vts
{

enum class Srs
{
    Physical,
    Navigation,
    Public,
};

class VTS_API Map
{
public:
    Map(const class MapCreateOptions &options);
    virtual ~Map();
    
    void setMapConfigPath(const std::string &mapConfigPath,
                          const std::string &authPath = "");
    const std::string getMapConfigPath() const;
    void purgeTraverseCache(bool hard);
    
    /// returns whether the map config has been downloaded
    /// and parsed successfully
    /// most other functions will not work until this is ready
    bool isMapConfigReady() const;
    /// returns whether the map has had all resources needed for complete
    /// render
    bool isMapRenderComplete() const;
    /// returns percentage estimation of progress till complete render
    double getMapRenderProgress() const;
    
    void dataInitialize(class Fetcher *fetcher);
    bool dataTick();
    void dataFinalize();

    void renderInitialize();
    void renderTick(uint32 width, uint32 height);
    void renderFinalize();
    
    class MapCallbacks &callbacks();
    class MapStatistics &statistics();
    class MapOptions &options();
    class MapDraws &draws();

    void pan(const Point &value);
    void rotate(const Point &value);
    void setPositionSubjective(bool subjective, bool convert);
    bool getPositionSubjective() const;
    void setPositionPoint(const Point &point); /// navigation srs
    const Point getPositionPoint() const; /// navigation srs
    void setPositionRotation(const Point &point); /// degrees
    const Point getPositionRotation() const; /// degrees
    void setPositionViewExtent(double viewExtent); /// physical length
    double getPositionViewExtent() const; /// physical length
    void setPositionFov(double fov); /// degrees
    double getPositionFov() const; /// degrees
    const std::string getPositionJson() const;
    void setPositionJson(const std::string &position);
    void resetPositionAltitude();
    void setAutorotate(double rotate);
    double getAutorotate() const;
    
    const Point convert(const Point &point, Srs from, Srs to) const;
    
    const std::vector<std::string> getResourceSurfaces() const;
    const std::vector<std::string> getResourceBoundLayers() const;
    const std::vector<std::string> getResourceFreeLayers() const;
    
    const std::vector<std::string> getViewNames() const;
    const std::string getViewCurrent() const;
    void setViewCurrent(const std::string &name);
    void getViewData(const std::string &name, class MapView &view) const;
    void setViewData(const std::string &name, const class MapView &view);
    const std::string getViewJson(const std::string &name) const;
    void setViewJson(const std::string &name, const std::string &view);
    
    void printDebugInfo();

private:
    std::shared_ptr<class MapImpl> impl;
};

} // namespace vts

#endif