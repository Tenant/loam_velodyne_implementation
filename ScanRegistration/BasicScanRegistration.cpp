#include <pcl/filters/voxel_grid.h>

#include "BasicScanRegistration.h"
#include "math_utils.h"

namespace loam
{

RegistrationParams::RegistrationParams(const float& scanPeriod_,
                                       const int& imuHistorySize_,
                                       const int& nFeatureRegions_,
                                       const int& curvatureRegion_,
                                       const int& maxCornerSharp_,
                                       const int& maxSurfaceFlat_,
                                       const float& lessFlatFilterSize_,
                                       const float& surfaceCurvatureThreshold_)
    : scanPeriod(scanPeriod_),
      imuHistorySize(imuHistorySize_),
      nFeatureRegions(nFeatureRegions_),
      curvatureRegion(curvatureRegion_),
      maxCornerSharp(maxCornerSharp_),
      maxCornerLessSharp(10 * maxCornerSharp_),
      maxSurfaceFlat(maxSurfaceFlat_),
      lessFlatFilterSize(lessFlatFilterSize_),
      surfaceCurvatureThreshold(surfaceCurvatureThreshold_)
{};

void BasicScanRegistration::processScanlines(const Time& scanTime,
        std::vector<pcl::PointCloud<pcl::PointXYZI>> const& laserCloudScans,
        pcl::PointCloud<pcl::PointXYZI>& cornerPointsSharp,
        pcl::PointCloud<pcl::PointXYZI>& cornerPointsLessSharp,
        pcl::PointCloud<pcl::PointXYZI>& surfPointsLessFlat,
        pcl::PointCloud<pcl::PointXYZI>& surfPointsFlat)
{
  // reset internal buffers and set IMU start state based on current scan time
  reset(scanTime);  

  // construct sorted full resolution cloud
  size_t cloudSize = 0;
  for (int i = 0; i < laserCloudScans.size(); i++) {
    _laserCloud += laserCloudScans[i];

    IndexRange range(cloudSize, 0);
    cloudSize += laserCloudScans[i].size();
    range.second = cloudSize > 0 ? cloudSize - 1 : 0; /* 使用range存储每根激光线起始和终止激光点的索引 */
    _scanIndices.push_back(range);
  }

  extractFeatures();
  updateIMUTransform();

  cornerPointsSharp = _cornerPointsSharp;
  cornerPointsLessSharp = _cornerPointsLessSharp;
  surfPointsLessFlat = _surfacePointsLessFlat;
  surfPointsFlat = _surfacePointsFlat;
//  for(int i = 0; i < _cornerPointsSharp.points.size(); i++){
//    pcl::PointXYZI single_laserCloudIn;
//    single_laserCloudIn.x = _cornerPointsSharp.points[i].x;
//    cornerPointsSharp.points.push_back();
//  }

}

void BasicScanRegistration::reset(const Time& scanTime)
{
  _scanTime = scanTime;

  // re-initialize IMU start index and state
  _imuIdx = 0;
  if (hasIMUData()) {
    /* 计算该帧激光数据扫描开始时对应的imu状态 */
    interpolateIMUStateFor(0, _imuStart);
  }

  // clear internal cloud buffers at the beginning of a sweep
  if (true/*newSweep*/) {
    _sweepStart = scanTime;

    // clear cloud buffers
    _laserCloud.clear();
    _cornerPointsSharp.clear();
    _cornerPointsLessSharp.clear();
    _surfacePointsFlat.clear();
    _surfacePointsLessFlat.clear();

    // clear scan indices vector
    _scanIndices.clear();
  }
}

void BasicScanRegistration::projectPointToStartOfSweep(pcl::PointXYZI& point, float relTime)
{
  // project point to the start of the sweep using corresponding IMU data
  if (hasIMUData())
  {
    setIMUTransformFor(relTime);
    transformToStartIMU(point);
  }
}

void BasicScanRegistration::setIMUTransformFor(const float& relTime)
{
  interpolateIMUStateFor(relTime, _imuCur);

  float relSweepTime = toSec(_scanTime - _sweepStart) + relTime;
  _imuPositionShift = _imuCur.position - _imuStart.position - _imuStart.velocity * relSweepTime;
}

void BasicScanRegistration::transformToStartIMU(pcl::PointXYZI& point)
{
  // rotate point to global IMU system
  rotateZXY(point, _imuCur.roll, _imuCur.pitch, _imuCur.yaw);

  // add global IMU position shift
  point.x += _imuPositionShift.x();
  point.y += _imuPositionShift.y();
  point.z += _imuPositionShift.z();

  // rotate point back to local IMU system relative to the start IMU state
  rotateYXZ(point, -_imuStart.yaw, -_imuStart.pitch, -_imuStart.roll);
}

void BasicScanRegistration::interpolateIMUStateFor(const float &relTime, IMUState &outputState)
{
  double timeDiff = toSec(_scanTime - _imuHistory[_imuIdx].stamp) + relTime;
  while (_imuIdx < _imuHistory.size() - 1 && timeDiff > 0) {
    _imuIdx++;
    timeDiff = toSec(_scanTime - _imuHistory[_imuIdx].stamp) + relTime;
  }

  if (_imuIdx == 0 || timeDiff > 0) {
    outputState = _imuHistory[_imuIdx];
  } else {
    float ratio = -timeDiff / toSec(_imuHistory[_imuIdx].stamp - _imuHistory[_imuIdx - 1].stamp);
    IMUState::interpolate(_imuHistory[_imuIdx], _imuHistory[_imuIdx - 1], ratio, outputState);
  }
}

void BasicScanRegistration::extractFeatures(const uint16_t& beginIdx)
{
  // extract features from individual scans
  size_t nScans = _scanIndices.size();
  for (size_t i = beginIdx; i < nScans; i++) {
    pcl::PointCloud<pcl::PointXYZI>::Ptr surfPointsLessFlatScan(new pcl::PointCloud<pcl::PointXYZI>);
    size_t scanStartIdx = _scanIndices[i].first;
    size_t scanEndIdx = _scanIndices[i].second;

    // skip empty scans
    if (scanEndIdx <= scanStartIdx + 2 * _config.curvatureRegion) {
      continue;
    }

    /* 判断该激光线上的点是否有效并标记无效点 */
    setScanBuffersFor(scanStartIdx, scanEndIdx);

    // extract features from equally sized scan regions
    for (int j = 0; j < _config.nFeatureRegions; j++) {
      size_t sp = ((scanStartIdx + _config.curvatureRegion) * (_config.nFeatureRegions - j)
                   + (scanEndIdx - _config.curvatureRegion) * j) / _config.nFeatureRegions;
      size_t ep = ((scanStartIdx + _config.curvatureRegion) * (_config.nFeatureRegions - 1 - j)
                   + (scanEndIdx - _config.curvatureRegion) * (j + 1)) / _config.nFeatureRegions - 1;

      // skip empty regions
      if (ep <= sp) {
        continue;
      }

      size_t regionSize = ep - sp + 1;

      /* 计算该region点的曲率并排序 */
      setRegionBuffersFor(sp, ep);

      // extract corner features
      int largestPickedNum = 0;
      for (size_t k = regionSize; k > 0 && largestPickedNum < _config.maxCornerLessSharp;) {
        size_t idx = _regionSortIndices[--k];
        size_t scanIdx = idx - scanStartIdx;
        size_t regionIdx = idx - sp;

        if (_scanNeighborPicked[scanIdx] == 0 &&
            _regionCurvature[regionIdx] > _config.surfaceCurvatureThreshold) {

          largestPickedNum++;
          if (largestPickedNum <= _config.maxCornerSharp) {
            _regionLabel[regionIdx] = CORNER_SHARP;
            _cornerPointsSharp.push_back(_laserCloud[idx]);
          } else {
            _regionLabel[regionIdx] = CORNER_LESS_SHARP;
          }
          _cornerPointsLessSharp.push_back(_laserCloud[idx]);

          markAsPicked(idx, scanIdx);
        }
      }

      // extract flat surface features
      int smallestPickedNum = 0;
      for (int k = 0; k < regionSize && smallestPickedNum < _config.maxSurfaceFlat; k++) {
        size_t idx = _regionSortIndices[k];
        size_t scanIdx = idx - scanStartIdx;
        size_t regionIdx = idx - sp;

        if (_scanNeighborPicked[scanIdx] == 0 &&
            _regionCurvature[regionIdx] < _config.surfaceCurvatureThreshold) {

          smallestPickedNum++;
          _regionLabel[regionIdx] = SURFACE_FLAT;
          _surfacePointsFlat.push_back(_laserCloud[idx]);

          markAsPicked(idx, scanIdx);
        }
      }

      // extract less flat surface features
      for (int k = 0; k < regionSize; k++) {
        if (_regionLabel[k] <= SURFACE_LESS_FLAT) {
          surfPointsLessFlatScan->push_back(_laserCloud[sp + k]);
        }
      }
    }

    // down size less flat surface point cloud of current scan
    pcl::PointCloud<pcl::PointXYZI> surfPointsLessFlatScanDS;
    pcl::VoxelGrid<pcl::PointXYZI> downSizeFilter;
    downSizeFilter.setInputCloud(surfPointsLessFlatScan);
    downSizeFilter.setLeafSize(_config.lessFlatFilterSize, _config.lessFlatFilterSize, _config.lessFlatFilterSize);
    downSizeFilter.filter(surfPointsLessFlatScanDS);

    _surfacePointsLessFlat += surfPointsLessFlatScanDS;
  }
}

void BasicScanRegistration::updateIMUTransform()
{
  _imuTrans[0].x = _imuStart.pitch.rad();
  _imuTrans[0].y = _imuStart.yaw.rad();
  _imuTrans[0].z = _imuStart.roll.rad();

  _imuTrans[1].x = _imuCur.pitch.rad();
  _imuTrans[1].y = _imuCur.yaw.rad();
  _imuTrans[1].z = _imuCur.roll.rad();

  Vector3 imuShiftFromStart = _imuPositionShift;
  rotateYXZ(imuShiftFromStart, -_imuStart.yaw, -_imuStart.pitch, -_imuStart.roll);

  _imuTrans[2].x = imuShiftFromStart.x();
  _imuTrans[2].y = imuShiftFromStart.y();
  _imuTrans[2].z = imuShiftFromStart.z();

  Vector3 imuVelocityFromStart = _imuCur.velocity - _imuStart.velocity;
  rotateYXZ(imuVelocityFromStart, -_imuStart.yaw, -_imuStart.pitch, -_imuStart.roll);

  _imuTrans[3].x = imuVelocityFromStart.x();
  _imuTrans[3].y = imuVelocityFromStart.y();
  _imuTrans[3].z = imuVelocityFromStart.z();
}

void BasicScanRegistration::setRegionBuffersFor(const size_t& startIdx, const size_t& endIdx)
{
  // resize buffers
  size_t regionSize = endIdx - startIdx + 1;
  _regionCurvature.resize(regionSize);
  _regionSortIndices.resize(regionSize);
  _regionLabel.assign(regionSize, SURFACE_LESS_FLAT);

  // calculate point curvatures and reset sort indices
  float pointWeight = -2 * _config.curvatureRegion;

  for (size_t i = startIdx, regionIdx = 0; i <= endIdx; i++, regionIdx++) {
    float diffX = pointWeight * _laserCloud[i].x;
    float diffY = pointWeight * _laserCloud[i].y;
    float diffZ = pointWeight * _laserCloud[i].z;

    for (int j = 1; j <= _config.curvatureRegion; j++) {
      diffX += _laserCloud[i + j].x + _laserCloud[i - j].x;
      diffY += _laserCloud[i + j].y + _laserCloud[i - j].y;
      diffZ += _laserCloud[i + j].z + _laserCloud[i - j].z;
    }

    _regionCurvature[regionIdx] = diffX * diffX + diffY * diffY + diffZ * diffZ;
    _regionSortIndices[regionIdx] = i;
  }

  // sort point curvatures
  for (size_t i = 1; i < regionSize; i++) {
    for (size_t j = i; j >= 1; j--) {
      if (_regionCurvature[_regionSortIndices[j] - startIdx] < _regionCurvature[_regionSortIndices[j - 1] - startIdx]) {
        std::swap(_regionSortIndices[j], _regionSortIndices[j - 1]);
      }
    }
  }
}

void BasicScanRegistration::setScanBuffersFor(const size_t& startIdx, const size_t& endIdx)
{
  // resize buffers
  size_t scanSize = endIdx - startIdx + 1;
  _scanNeighborPicked.assign(scanSize, 0);

  // mark unreliable points as picked
  for (size_t i = startIdx + _config.curvatureRegion; i < endIdx - _config.curvatureRegion; i++) {
    const pcl::PointXYZI& previousPoint = (_laserCloud[i - 1]);
    const pcl::PointXYZI& point = (_laserCloud[i]);
    const pcl::PointXYZI& nextPoint = (_laserCloud[i + 1]);

    float diffNext = calcSquaredDiff(nextPoint, point);

    if (diffNext > 0.1) {
      float depth1 = calcPointDistance(point);
      float depth2 = calcPointDistance(nextPoint);

      if (depth1 > depth2) {
        float weighted_distance = std::sqrt(calcSquaredDiff(nextPoint, point, depth2 / depth1)) / depth2;

        if (weighted_distance < 0.1) {
          std::fill_n(&_scanNeighborPicked[i - startIdx - _config.curvatureRegion], _config.curvatureRegion + 1, 1);

          continue;
        }
      } else {
        float weighted_distance = std::sqrt(calcSquaredDiff(point, nextPoint, depth1 / depth2)) / depth1;

        if (weighted_distance < 0.1) {
          std::fill_n(&_scanNeighborPicked[i - startIdx + 1], _config.curvatureRegion + 1, 1);
        }
      }
    }

    float diffPrevious = calcSquaredDiff(point, previousPoint);
    float dis = calcSquaredPointDistance(point);

    if (diffNext > 0.0002 * dis && diffPrevious > 0.0002 * dis) {
      _scanNeighborPicked[i - startIdx] = 1;
    }
  }
  // Ddbug Output
  /*
  printf("%.6f\n",toSec(_scanTime));
  char filename[50];
  sprintf(filename, "/home/sukie/Desktop/data/pcl-%ld.txt",1);
  FILE *fp;
  fp=fopen(filename,"w");
  for(int i=startIdx;i<endIdx;i++){
    fprintf(fp,"%.3f, %.3f, %.3f, %.6f\n",_laserCloud[i].x,_laserCloud[i].y,_laserCloud[i].z,_laserCloud[i].intensity);
  }
  fclose(fp);
  */
}

void BasicScanRegistration::markAsPicked(const size_t& cloudIdx, const size_t& scanIdx)
{
  _scanNeighborPicked[scanIdx] = 1;

  for (int i = 1; i <= _config.curvatureRegion; i++) {
    if (calcSquaredDiff(_laserCloud[cloudIdx + i], _laserCloud[cloudIdx + i - 1]) > 0.05) {
      break;
    }

    _scanNeighborPicked[scanIdx + i] = 1;
  }

  for (int i = 1; i <= _config.curvatureRegion; i++) {
    if (calcSquaredDiff(_laserCloud[cloudIdx - i], _laserCloud[cloudIdx - i + 1]) > 0.05) {
      break;
    }

    _scanNeighborPicked[scanIdx - i] = 1;
  }
}

}