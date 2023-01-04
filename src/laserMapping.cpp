// This is an advanced implementation of the algorithm described in the
// following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// Modifier: Livox               dev@livoxtech.com

// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#include <omp.h> 
#include <mutex>
#include <math.h>
#include <thread>
#include <fstream>
#include <csignal>
#include <unistd.h>
#include <Python.h>
#include <so3_math.h>
#include <ros/ros.h>
#include <Eigen/Core>
#include "IMU_Processing.hpp"
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <visualization_msgs/Marker.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/pcd_io.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <geometry_msgs/Vector3.h>

#include "preprocess.h"
#include <ikd-Tree/ikd_Tree.h>

#define INIT_TIME           (0.1)
#define LASER_POINT_COV     (0.001)
#define MAXN                (720000)
#define PUBFRAME_PERIOD     (20)

/*** Time Log Variables ***/
double kdtree_incremental_time = 0.0, kdtree_search_time = 0.0, kdtree_delete_time = 0.0;//定义的一些时间
double T1[MAXN], s_plot[MAXN], s_plot2[MAXN], s_plot3[MAXN], s_plot4[MAXN], s_plot5[MAXN], s_plot6[MAXN], s_plot7[MAXN], s_plot8[MAXN], s_plot9[MAXN], s_plot10[MAXN], s_plot11[MAXN];
double match_time = 0, solve_time = 0, solve_const_H_time = 0;//定义的一些时间
int    kdtree_size_st = 0, kdtree_size_end = 0, add_point_size = 0, kdtree_delete_counter = 0;
bool   runtime_pos_log = false, pcd_save_en = false, time_sync_en = false, extrinsic_est_en = true, path_en = true;
/**************************/

float res_last[100000] = {0.0};
float DET_RANGE = 50.0f;//300.0f gai 只是用来判断是否移动地图
const float MOV_THRESHOLD = 1.5f;

mutex mtx_buffer;  // 互斥锁
condition_variable sig_buffer;  // 条件变量

string root_dir = ROOT_DIR;
string map_file_path, lid_topic, imu_topic;//地图文件路径，雷达话题，imu话题

double res_mean_last = 0.05, total_residual = 0.0;
double last_timestamp_lidar = 0, last_timestamp_imu = -1.0;//上一帧数据结束时间
double gyr_cov = 0.1, acc_cov = 0.1, b_gyr_cov = 0.0001, b_acc_cov = 0.0001;
double filter_size_corner_min = 0, filter_size_surf_min = 0, filter_size_map_min = 0, fov_deg = 0;
double cube_len = 0, HALF_FOV_COS = 0, FOV_DEG = 0, total_distance = 0, lidar_end_time = 0, first_lidar_time = 0.0;
// scan_count：接收到的激光雷达Msg的总数，publish_count：接收到的IMU的Msg的总数
int    effct_feat_num = 0, time_log_counter = 0, scan_count = 0, publish_count = 0;
int    iterCount = 0, feats_down_size = 0, NUM_MAX_ITERATIONS = 0, laserCloudValidNum = 0, pcd_save_interval = -1, pcd_index = 0;
bool   point_selected_surf[100000] = {0};
// lidar_pushed：用于判断激光雷达数据是否从缓存队列中拿到meas中
bool   lidar_pushed, flg_first_scan = true, flg_exit = false, flg_EKF_inited;
bool   scan_pub_en = false, dense_pub_en = false, scan_body_pub_en = false;

vector<vector<int>>  pointSearchInd_surf; 
vector<BoxPointType> cub_needrm;        //
vector<PointVector>  Nearest_Points; 
vector<double>       extrinT(3, 0.0);   //3个0.0的数组
vector<double>       extrinR(9, 0.0);   //9个0.0的数组
deque<double>                     time_buffer;   // 激光雷达数据时间戳缓存队列
deque<PointCloudXYZI::Ptr>        lidar_buffer;  // 激光雷达数据缓存队列
deque<sensor_msgs::Imu::ConstPtr> imu_buffer;    // IMU数据缓存队列

PointCloudXYZI::Ptr featsFromMap(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_undistort(new PointCloudXYZI());//用于接受imu处理、前向后向传播后的数据
PointCloudXYZI::Ptr feats_down_body(new PointCloudXYZI());//用于接受将采样后的数据
PointCloudXYZI::Ptr feats_down_world(new PointCloudXYZI());
PointCloudXYZI::Ptr normvec(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr laserCloudOri(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr corr_normvect(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr _featsArray;//一个点云数组，后续好像没用到

pcl::VoxelGrid<PointType> downSizeFilterSurf;//pcl 体速滤波类
pcl::VoxelGrid<PointType> downSizeFilterMap;

KD_TREE<PointType> ikdtree;

V3F XAxisPoint_body(LIDAR_SP_LEN, 0.0, 0.0);
V3F XAxisPoint_world(LIDAR_SP_LEN, 0.0, 0.0);
V3D euler_cur;
V3D position_last(Zero3d);
V3D Lidar_T_wrt_IMU(Zero3d);//创建一个0向量
M3D Lidar_R_wrt_IMU(Eye3d);//创建一个0矩阵

/*** EKF inputs and output ***/
MeasureGroup Measures;
esekfom::esekf<state_ikfom, 12, input_ikfom> kf;//TODO  KF输入
state_ikfom state_point;//ikfom迭代卡尔曼滤波在流形上的使用，输出
vect3 pos_lid;

nav_msgs::Path path;
nav_msgs::Odometry odomAftMapped;
geometry_msgs::Quaternion geoQuat;
geometry_msgs::PoseStamped msg_body_pose;

shared_ptr<Preprocess> p_pre(new Preprocess());  // 定义指向激光雷达数据的预处理类Preprocess的智能指针
shared_ptr<ImuProcess> p_imu(new ImuProcess());  // 定义指向IMU数据预处理类ImuProcess的智能指针

//程序退出函数
void SigHandle(int sig)
{
    flg_exit = true;
    ROS_WARN("catch sig %d", sig);
    sig_buffer.notify_all();  // 唤醒所有线程
}

//打印状态
inline void dump_lio_state_to_log(FILE *fp)  
{
    V3D rot_ang(Log(state_point.rot.toRotationMatrix()));
    fprintf(fp, "%lf ", Measures.lidar_beg_time - first_lidar_time);//预处理运行时间
    fprintf(fp, "%lf %lf %lf ", rot_ang(0), rot_ang(1), rot_ang(2));                   // Angle
    fprintf(fp, "%lf %lf %lf ", state_point.pos(0), state_point.pos(1), state_point.pos(2)); // Pos  
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                        // omega  
    fprintf(fp, "%lf %lf %lf ", state_point.vel(0), state_point.vel(1), state_point.vel(2)); // Vel  
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                        // Acc  
    fprintf(fp, "%lf %lf %lf ", state_point.bg(0), state_point.bg(1), state_point.bg(2));    // Bias_g  
    fprintf(fp, "%lf %lf %lf ", state_point.ba(0), state_point.ba(1), state_point.ba(2));    // Bias_a  
    fprintf(fp, "%lf %lf %lf ", state_point.grav[0], state_point.grav[1], state_point.grav[2]); // Bias_a  
    fprintf(fp, "\r\n");  
    fflush(fp);
}

//把点从body系转到world系
void pointBodyToWorld_ikfom(PointType const * const pi, PointType * const po, state_ikfom &s)
{
    V3D p_body(pi->x, pi->y, pi->z);
    //下面式子最里面的括号是从雷达到IMU坐标系 然后从IMU转到世界坐标系 p2=r*p1+t
    V3D p_global(s.rot * (s.offset_R_L_I*p_body + s.offset_T_L_I) + s.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

//把点从body系转到world系 和上面其实差不多 
void pointBodyToWorld(PointType const * const pi, PointType * const po)
{
    V3D p_body(pi->x, pi->y, pi->z);
    //这个时候已经完成了配准，state_point里是当前RT关系？
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

//把点从body系转到world系 和上面其实大同小异
template<typename T>
void pointBodyToWorld(const Matrix<T, 3, 1> &pi, Matrix<T, 3, 1> &po)
{
    V3D p_body(pi[0], pi[1], pi[2]);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po[0] = p_global(0);
    po[1] = p_global(1);
    po[2] = p_global(2);
}

//把点从body系转到world系
void RGBpointBodyToWorld(PointType const * const pi, PointType * const po)
{
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

//把点从Lidar系转到IMU系
void RGBpointBodyLidarToIMU(PointType const * const pi, PointType * const po)
{
    V3D p_body_lidar(pi->x, pi->y, pi->z);
    V3D p_body_imu(state_point.offset_R_L_I*p_body_lidar + state_point.offset_T_L_I);

    po->x = p_body_imu(0);
    po->y = p_body_imu(1);
    po->z = p_body_imu(2);
    po->intensity = pi->intensity;
}

//得到被剔除的点
void points_cache_collect()
{
    PointVector points_history;
    ikdtree.acquire_removed_points(points_history);
    // for (int i = 0; i < points_history.size(); i++) _featsArray->push_back(points_history[i]);
}

BoxPointType LocalMap_Points;
bool Localmap_Initialized = false;
//在激光雷达视场中分割地图
void lasermap_fov_segment()
{
    cub_needrm.clear();//局部地图清空？
    kdtree_delete_counter = 0;
    kdtree_delete_time = 0.0;    
    pointBodyToWorld(XAxisPoint_body, XAxisPoint_world);
    V3D pos_LiD = pos_lid;
    //若未初始化 以当前雷达点为中心 长宽高200*200*200的局部地图
    if (!Localmap_Initialized){
        
        for (int i = 0; i < 3; i++){
            LocalMap_Points.vertex_min[i] = pos_LiD(i) - cube_len / 2.0;
            LocalMap_Points.vertex_max[i] = pos_LiD(i) + cube_len / 2.0;
        }
        Localmap_Initialized = true;
        return;
    }

    float dist_to_map_edge[3][2];
    bool need_move = false;
    //当前雷达系中心到各个地图边缘的距离
    for (int i = 0; i < 3; i++){
        dist_to_map_edge[i][0] = fabs(pos_LiD(i) - LocalMap_Points.vertex_min[i]);
        dist_to_map_edge[i][1] = fabs(pos_LiD(i) - LocalMap_Points.vertex_max[i]);
        //如果距离边缘过近 则需要进行地图的挪动
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE || dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE) need_move = true;
    }
    
    if (!need_move) return;//不需要挪动就直接退回了
    //挪动地图
    BoxPointType New_LocalMap_Points, tmp_boxpoints;
    New_LocalMap_Points = LocalMap_Points;
    float mov_dist = max((cube_len - 2.0 * MOV_THRESHOLD * DET_RANGE) * 0.5 * 0.9, double(DET_RANGE * (MOV_THRESHOLD -1)));
    for (int i = 0; i < 3; i++){
        tmp_boxpoints = LocalMap_Points;
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE){
            New_LocalMap_Points.vertex_max[i] -= mov_dist;
            New_LocalMap_Points.vertex_min[i] -= mov_dist;
            tmp_boxpoints.vertex_min[i] = LocalMap_Points.vertex_max[i] - mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        } else if (dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE){
            New_LocalMap_Points.vertex_max[i] += mov_dist;
            New_LocalMap_Points.vertex_min[i] += mov_dist;
            tmp_boxpoints.vertex_max[i] = LocalMap_Points.vertex_min[i] + mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        }
    }
    LocalMap_Points = New_LocalMap_Points;

    points_cache_collect();//得到被剔除的点
    double delete_begin = omp_get_wtime();//
    if(cub_needrm.size() > 0) kdtree_delete_counter = ikdtree.Delete_Point_Boxes(cub_needrm);//
    kdtree_delete_time = omp_get_wtime() - delete_begin;//计算处理时间
}


/* rosmsg info sensor_msgs/PointCloud2
 std_msgs/Header header头
    uint32 seq          序列
    time stamp          时间
    string frame_id     坐标系
  uint32 height         维度
  uint32 width          宽度
 sensor_msgs/PointField[]fields 点云域
    uint8 INT8=1
    uint8 UINT8=2
    uint8 INT16=3
    uint8 UINT16=4
    uint8 INT32=5
    uint8 UINT32=6
    uint8 FLOAT32=7
    uint8 FLOAT64=8
    string name         坐标轴
    uint32 offset       偏移量
    uint8 datatype      数据类型 7
    uint32 count        计数
  bool is_bigendian   这是数据 bigendian false
  uint32 point_step   点的长度（以字节为单位）
  uint32 row_step     行的长度（以字节为单位）
  uint8[] data        实际点数据，大小为(row_step*height)
  bool is_dense       ture如果没有无效点
*/
//旋转雷达回调函数，shared ptr传入一帧点云，进行预处理之后将数据存入雷达缓存区
void standard_pcl_cbk(const sensor_msgs::PointCloud2::ConstPtr &msg) 
{
    /* 利用锁来保护共享变量，在这里共享变量是多个线程都能对其进行访问，所以就是共享变量啦
     mtx.lock()：调用该函数的线程尝试加锁。如果上锁不成功，即：其它线程已经上锁且未释放，则当前线程block。如果上锁成功，则执行后面的操作，操作完成后要调用mtx.unlock()释放锁，否则会导致死锁的产生
     */
    mtx_buffer.lock();//打开进程锁
    scan_count ++;//接收到的雷达信息+1，是雷达发布出的每一帧点云
    double preprocess_start_time = omp_get_wtime();//omp_get_wtime函数可以返回从某个特殊点所经过的时间，单位秒，返回当前时间
    if (msg->header.stamp.toSec() < last_timestamp_lidar)//lidar loop back, clear buffer
    {
        ROS_ERROR("lidar loop back, clear buffer");
        lidar_buffer.clear();
    }

    PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());//新建一个点格式为PointXYZI的智能指针
    p_pre->process(msg, ptr);//将雷达发布出的每一帧点云进行预处理然后保存给ptr
    lidar_buffer.push_back(ptr);//将雷达信息存入队列缓存区
    time_buffer.push_back(msg->header.stamp.toSec());//将当前帧时间存入时间缓存区
    last_timestamp_lidar = msg->header.stamp.toSec();//将当前帧时间付给上一帧时间变量
    s_plot11[scan_count] = omp_get_wtime() - preprocess_start_time;//TODO该帧处理时间？
    mtx_buffer.unlock();//关闭进程锁
    sig_buffer.notify_all();  // 唤醒所有线程
}

double timediff_lidar_wrt_imu = 0.0;    //时间差-雷达-imu
bool   timediff_set_flg = false;        //时间同步flag，false表示未进行时间同步，true表示已经进行过时间同步
/* Header header	#imu头信息

geometry_msgs/Quaternion orientation	#四元数[x,y,z,w]
float64[9] orientation_covariance		# 对应协方差矩阵 Row major about x, y, z axes

geometry_msgs/Vector3 angular_velocity	#角速度[x,y,z]轴
float64[9] angular_velocity_covariance	# 对应协方差矩阵，Row major(行主序) about x, y, z axes

geometry_msgs/Vector3 linear_acceleration	#线性加速度[x,y,z]
float64[9] linear_acceleration_covariance	# 对应协方差矩阵 Row major x, y z 
 */
/** @brief 订阅器sub_imu的回调函数：接收IMU数据，进行时间戳对齐，将IMU数据保存到IMU数据缓存队列中
 * 
 * @param msg_in IMU Msg
 * @return void
 */
void imu_cbk(const sensor_msgs::Imu::ConstPtr &msg_in) 
{
    publish_count ++;
    // cout<<"IMU got at: "<<msg_in->header.stamp.toSec()<<endl;
    sensor_msgs::Imu::Ptr msg(new sensor_msgs::Imu(*msg_in));//新建指针msg接受msg-in

    // 将IMU和激光雷达点云的时间戳对齐
    if (abs(timediff_lidar_wrt_imu) > 0.1 && time_sync_en)
    {
        msg->header.stamp = \
        ros::Time().fromSec(timediff_lidar_wrt_imu + msg_in->header.stamp.toSec());
    }
    double timestamp = msg->header.stamp.toSec();
    mtx_buffer.lock();  // 互斥锁
    // 如果当前IMU的时间戳小于上一个时刻IMU的时间戳，则IMU数据有误，将IMU数据缓存队列清空
    if (timestamp < last_timestamp_imu)
    {
        ROS_WARN("imu loop back, clear buffer");
        imu_buffer.clear();
    }
    last_timestamp_imu = timestamp;
    // 将当前的IMU数据保存到IMU数据缓存队列中
    imu_buffer.push_back(msg);
    mtx_buffer.unlock();
    sig_buffer.notify_all();  // 唤醒所有线程
}

double lidar_mean_scantime = 0.0;
int    scan_num = 0;
/**@brief 将激光雷达点云数据和IMU数据从缓存队列中取出，进行时间对齐，并保存到meas中
 * 
 * @param meas  用于保存当前正在处理的激光雷达数据和IMU数据
 * @return true 
 * @return false 
 */
bool sync_packages(MeasureGroup &meas)
{

    if (lidar_buffer.empty() || imu_buffer.empty()) {
        return false;
    }

    /*** push a lidar scan ***/
    if(!lidar_pushed)
    {
        // 从激光雷达点云缓存队列中取出点云数据，放到meas中
        meas.lidar = lidar_buffer.front();
        // 记录该次雷达扫描的开始时间
        meas.lidar_beg_time = time_buffer.front();
        // 计算该次雷达扫描的结束时间
        if (meas.lidar->points.size() <= 1) // time too little
        {
            lidar_end_time = meas.lidar_beg_time + lidar_mean_scantime;
            ROS_WARN("Too few input point cloud!\n");
        }
        else if (meas.lidar->points.back().curvature / double(1000) < 0.5 * lidar_mean_scantime)
        {
            lidar_end_time = meas.lidar_beg_time + lidar_mean_scantime;
        }
        else
        {
            scan_num ++;
            lidar_end_time = meas.lidar_beg_time + meas.lidar->points.back().curvature / double(1000);
            lidar_mean_scantime += (meas.lidar->points.back().curvature / double(1000) - lidar_mean_scantime) / scan_num;
        }

        meas.lidar_end_time = lidar_end_time;

        lidar_pushed = true;
    }

    if (last_timestamp_imu < lidar_end_time)
    {
        return false;
    }

    /*** push imu data, and pop from imu buffer ***/
    // 从IMU缓存队列中取出IMU数据，放到meas中
    double imu_time = imu_buffer.front()->header.stamp.toSec();
    meas.imu.clear();
    // 拿出lidar_beg_time到lidar_end_time之间的所有IMU数据
    while ((!imu_buffer.empty()) && (imu_time < lidar_end_time))
    {
        imu_time = imu_buffer.front()->header.stamp.toSec();
        if(imu_time > lidar_end_time) break;
        meas.imu.push_back(imu_buffer.front());
        imu_buffer.pop_front();
    }

    lidar_buffer.pop_front();
    time_buffer.pop_front();
    lidar_pushed = false;
    return true;
}

//向地图kdtree添加特征点
int process_increments = 0;
void map_incremental()
{
    PointVector PointToAdd;
    PointVector PointNoNeedDownsample;
    PointToAdd.reserve(feats_down_size);
    PointNoNeedDownsample.reserve(feats_down_size);
    for (int i = 0; i < feats_down_size; i++)
    {
        /* transform to world frame */
        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
        /* decide if need add to map */
        if (!Nearest_Points[i].empty() && flg_EKF_inited)
        {
            const PointVector &points_near = Nearest_Points[i];
            bool need_add = true;
            BoxPointType Box_of_Point;
            PointType downsample_result, mid_point; 
            mid_point.x = floor(feats_down_world->points[i].x/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            mid_point.y = floor(feats_down_world->points[i].y/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            mid_point.z = floor(feats_down_world->points[i].z/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            float dist  = calc_dist(feats_down_world->points[i],mid_point);
            if (fabs(points_near[0].x - mid_point.x) > 0.5 * filter_size_map_min && fabs(points_near[0].y - mid_point.y) > 0.5 * filter_size_map_min && fabs(points_near[0].z - mid_point.z) > 0.5 * filter_size_map_min){
                PointNoNeedDownsample.push_back(feats_down_world->points[i]);
                continue;
            }
            for (int readd_i = 0; readd_i < NUM_MATCH_POINTS; readd_i ++)
            {
                if (points_near.size() < NUM_MATCH_POINTS) break;
                if (calc_dist(points_near[readd_i], mid_point) < dist)
                {
                    need_add = false;
                    break;
                }
            }
            if (need_add) PointToAdd.push_back(feats_down_world->points[i]);
        }
        else
        {
            PointToAdd.push_back(feats_down_world->points[i]);
        }
    }

    double st_time = omp_get_wtime();
    add_point_size = ikdtree.Add_Points(PointToAdd, true);
    ikdtree.Add_Points(PointNoNeedDownsample, false); 
    add_point_size = PointToAdd.size() + PointNoNeedDownsample.size();
    kdtree_incremental_time = omp_get_wtime() - st_time;
}

PointCloudXYZI::Ptr pcl_wait_pub(new PointCloudXYZI(500000, 1));
PointCloudXYZI::Ptr pcl_wait_save(new PointCloudXYZI());

//发布当前的扫描转化到世界坐标系下的点云
void publish_frame_world(const ros::Publisher & pubLaserCloudFull)
{
    if(scan_pub_en)
    {
        PointCloudXYZI::Ptr laserCloudFullRes(dense_pub_en ? feats_undistort : feats_down_body);
        int size = laserCloudFullRes->points.size();
        PointCloudXYZI::Ptr laserCloudWorld( \
                        new PointCloudXYZI(size, 1));

        for (int i = 0; i < size; i++)
        {
            RGBpointBodyToWorld(&laserCloudFullRes->points[i], \
                                &laserCloudWorld->points[i]);
        }

        sensor_msgs::PointCloud2 laserCloudmsg;
        pcl::toROSMsg(*laserCloudWorld, laserCloudmsg);
        laserCloudmsg.header.stamp = ros::Time().fromSec(lidar_end_time);
        laserCloudmsg.header.frame_id = "camera_init";
        pubLaserCloudFull.publish(laserCloudmsg);
        publish_count -= PUBFRAME_PERIOD;
    }

    /**************** save map ****************/
    /* 1. make sure you have enough memories
    /* 2. noted that pcd save will influence the real-time performences **/
    if (pcd_save_en)
    {
        int size = feats_undistort->points.size();
        PointCloudXYZI::Ptr laserCloudWorld( \
                        new PointCloudXYZI(size, 1));

        for (int i = 0; i < size; i++)
        {
            RGBpointBodyToWorld(&feats_undistort->points[i], \
                                &laserCloudWorld->points[i]);
        }
        *pcl_wait_save += *laserCloudWorld;

        static int scan_wait_num = 0;
        scan_wait_num ++;
        if (pcl_wait_save->size() > 0 && pcd_save_interval > 0  && scan_wait_num >= pcd_save_interval)
        {
            pcd_index ++;
            string all_points_dir(string(string(ROOT_DIR) + "PCD/scans_") + to_string(pcd_index) + string(".pcd"));
            pcl::PCDWriter pcd_writer;
            cout << "current scan saved to /PCD/" << all_points_dir << endl;
            pcd_writer.writeBinary(all_points_dir, *pcl_wait_save);
            pcl_wait_save->clear();
            scan_wait_num = 0;
        }
    }
}

//发布当前的扫描转化到imu坐标系下的点云
void publish_frame_body(const ros::Publisher & pubLaserCloudFull_body)
{
    int size = feats_undistort->points.size();
    PointCloudXYZI::Ptr laserCloudIMUBody(new PointCloudXYZI(size, 1));

    for (int i = 0; i < size; i++)
    {
        RGBpointBodyLidarToIMU(&feats_undistort->points[i], \
                            &laserCloudIMUBody->points[i]);
    }

    sensor_msgs::PointCloud2 laserCloudmsg;
    pcl::toROSMsg(*laserCloudIMUBody, laserCloudmsg);
    laserCloudmsg.header.stamp = ros::Time().fromSec(lidar_end_time);
    laserCloudmsg.header.frame_id = "body";
    pubLaserCloudFull_body.publish(laserCloudmsg);
    publish_count -= PUBFRAME_PERIOD;
}

//没用到
void publish_effect_world(const ros::Publisher & pubLaserCloudEffect)
{
    PointCloudXYZI::Ptr laserCloudWorld( \
                    new PointCloudXYZI(effct_feat_num, 1));
    for (int i = 0; i < effct_feat_num; i++)
    {
        RGBpointBodyToWorld(&laserCloudOri->points[i], \
                            &laserCloudWorld->points[i]);
    }
    sensor_msgs::PointCloud2 laserCloudFullRes3;
    pcl::toROSMsg(*laserCloudWorld, laserCloudFullRes3);
    laserCloudFullRes3.header.stamp = ros::Time().fromSec(lidar_end_time);
    laserCloudFullRes3.header.frame_id = "camera_init";
    pubLaserCloudEffect.publish(laserCloudFullRes3);
}

//发布地图
void publish_map(const ros::Publisher & pubLaserCloudMap)
{
    sensor_msgs::PointCloud2 laserCloudMap;
    pcl::toROSMsg(*featsFromMap, laserCloudMap);
    laserCloudMap.header.stamp = ros::Time().fromSec(lidar_end_time);
    laserCloudMap.header.frame_id = "camera_init";
    pubLaserCloudMap.publish(laserCloudMap);
}
//设置输出的位姿
template<typename T>
void set_posestamp(T & out)
{
    out.pose.position.x = state_point.pos(0);
    out.pose.position.y = state_point.pos(1);
    out.pose.position.z = state_point.pos(2);
    out.pose.orientation.x = geoQuat.x;
    out.pose.orientation.y = geoQuat.y;
    out.pose.orientation.z = geoQuat.z;
    out.pose.orientation.w = geoQuat.w;
    
}
//发布里程计 发布tf变换
void publish_odometry(const ros::Publisher & pubOdomAftMapped)
{
    odomAftMapped.header.frame_id = "camera_init";
    odomAftMapped.child_frame_id = "body";
    odomAftMapped.header.stamp = ros::Time().fromSec(lidar_end_time);// ros::Time().fromSec(lidar_end_time);
    set_posestamp(odomAftMapped.pose);
    pubOdomAftMapped.publish(odomAftMapped);
    auto P = kf.get_P();
    for (int i = 0; i < 6; i ++)
    {
        int k = i < 3 ? i + 3 : i - 3;
        odomAftMapped.pose.covariance[i*6 + 0] = P(k, 3);
        odomAftMapped.pose.covariance[i*6 + 1] = P(k, 4);
        odomAftMapped.pose.covariance[i*6 + 2] = P(k, 5);
        odomAftMapped.pose.covariance[i*6 + 3] = P(k, 0);
        odomAftMapped.pose.covariance[i*6 + 4] = P(k, 1);
        odomAftMapped.pose.covariance[i*6 + 5] = P(k, 2);
    }

    static tf::TransformBroadcaster br;
    tf::Transform                   transform;
    tf::Quaternion                  q;
    transform.setOrigin(tf::Vector3(odomAftMapped.pose.pose.position.x, \
                                    odomAftMapped.pose.pose.position.y, \
                                    odomAftMapped.pose.pose.position.z));
    q.setW(odomAftMapped.pose.pose.orientation.w);
    q.setX(odomAftMapped.pose.pose.orientation.x);
    q.setY(odomAftMapped.pose.pose.orientation.y);
    q.setZ(odomAftMapped.pose.pose.orientation.z);
    transform.setRotation( q );
    br.sendTransform( tf::StampedTransform( transform, odomAftMapped.header.stamp, "camera_init", "body" ) );
}
//每隔10个发布一下位姿
void publish_path(const ros::Publisher pubPath)
{
    set_posestamp(msg_body_pose);
    msg_body_pose.header.stamp = ros::Time().fromSec(lidar_end_time);
    msg_body_pose.header.frame_id = "camera_init";

    /*** if path is too large, the rvis will crash ***/
    static int jjj = 0;
    jjj++;
    if (jjj % 10 == 0) 
    {
        path.poses.push_back(msg_body_pose);
        pubPath.publish(path);
    }
}

//TODO观测模型计算
void h_share_model(state_ikfom &s, esekfom::dyn_share_datastruct<double> &ekfom_data)
{
    double match_start = omp_get_wtime();
    laserCloudOri->clear(); 
    corr_normvect->clear(); 
    total_residual = 0.0; 

    /** closest surface search and residual computation **/
    #ifdef MP_EN
        omp_set_num_threads(MP_PROC_NUM);
        #pragma omp parallel for
    #endif
    for (int i = 0; i < feats_down_size; i++)
    {
        PointType &point_body  = feats_down_body->points[i]; 
        PointType &point_world = feats_down_world->points[i]; 

        /* transform to world frame */
        V3D p_body(point_body.x, point_body.y, point_body.z);
        V3D p_global(s.rot * (s.offset_R_L_I*p_body + s.offset_T_L_I) + s.pos);
        point_world.x = p_global(0);
        point_world.y = p_global(1);
        point_world.z = p_global(2);
        point_world.intensity = point_body.intensity;

        vector<float> pointSearchSqDis(NUM_MATCH_POINTS);

        auto &points_near = Nearest_Points[i];

        if (ekfom_data.converge)
        {
            /** Find the closest surfaces in the map **/
            ikdtree.Nearest_Search(point_world, NUM_MATCH_POINTS, points_near, pointSearchSqDis);
            point_selected_surf[i] = points_near.size() < NUM_MATCH_POINTS ? false : pointSearchSqDis[NUM_MATCH_POINTS - 1] > 5 ? false : true;
        }

        if (!point_selected_surf[i]) continue;

        VF(4) pabcd;
        point_selected_surf[i] = false;
        if (esti_plane(pabcd, points_near, 0.1f))
        {
            float pd2 = pabcd(0) * point_world.x + pabcd(1) * point_world.y + pabcd(2) * point_world.z + pabcd(3);
            float s = 1 - 0.9 * fabs(pd2) / sqrt(p_body.norm());

            if (s > 0.9)
            {
                point_selected_surf[i] = true;
                normvec->points[i].x = pabcd(0);
                normvec->points[i].y = pabcd(1);
                normvec->points[i].z = pabcd(2);
                normvec->points[i].intensity = pd2;
                res_last[i] = abs(pd2);
            }
        }
    }
    
    effct_feat_num = 0;

    for (int i = 0; i < feats_down_size; i++)
    {
        if (point_selected_surf[i])
        {
            laserCloudOri->points[effct_feat_num] = feats_down_body->points[i];
            corr_normvect->points[effct_feat_num] = normvec->points[i];
            total_residual += res_last[i];
            effct_feat_num ++;
        }
    }

    if (effct_feat_num < 1)
    {
        ekfom_data.valid = false;
        ROS_WARN("No Effective Points! \n");
        return;
    }

    res_mean_last = total_residual / effct_feat_num;
    match_time  += omp_get_wtime() - match_start;
    double solve_start_  = omp_get_wtime();
    
    /*** Computation of Measuremnt Jacobian matrix H and measurents vector ***/
    ekfom_data.h_x = MatrixXd::Zero(effct_feat_num, 12); //23
    ekfom_data.h.resize(effct_feat_num);

    for (int i = 0; i < effct_feat_num; i++)
    {
        const PointType &laser_p  = laserCloudOri->points[i];
        V3D point_this_be(laser_p.x, laser_p.y, laser_p.z);
        M3D point_be_crossmat;
        point_be_crossmat << SKEW_SYM_MATRX(point_this_be);
        V3D point_this = s.offset_R_L_I * point_this_be + s.offset_T_L_I;
        M3D point_crossmat;
        point_crossmat<<SKEW_SYM_MATRX(point_this);

        /*** get the normal vector of closest surface/corner ***/
        const PointType &norm_p = corr_normvect->points[i];
        V3D norm_vec(norm_p.x, norm_p.y, norm_p.z);

        /*** calculate the Measuremnt Jacobian matrix H ***/
        V3D C(s.rot.conjugate() *norm_vec);
        V3D A(point_crossmat * C);
        if (extrinsic_est_en)
        {
            V3D B(point_be_crossmat * s.offset_R_L_I.conjugate() * C); //s.rot.conjugate()*norm_vec);
            ekfom_data.h_x.block<1, 12>(i,0) << norm_p.x, norm_p.y, norm_p.z, VEC_FROM_ARRAY(A), VEC_FROM_ARRAY(B), VEC_FROM_ARRAY(C);
        }
        else
        {
            ekfom_data.h_x.block<1, 12>(i,0) << norm_p.x, norm_p.y, norm_p.z, VEC_FROM_ARRAY(A), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;
        }

        /*** Measuremnt: distance to the closest surface/corner ***/
        ekfom_data.h(i) = -norm_p.intensity;
    }
    solve_time += omp_get_wtime() - solve_start_;
}

//FAST_LIO2主函数
int main(int argc, char** argv)
{
    /*****************************初始化：读取参数、定义变量以及赋值*************************/

    // 初始化ros节点，节点名为laserMapping
    ros::init(argc, argv, "laserMapping");
    ros::NodeHandle nh;

    // 从参数服务器读取参数值赋给变量（包括launch文件和launch读取的yaml文件中的参数）
    nh.param<bool>("publish/path_en",path_en, true);                                // 是否发布路径的topic
    nh.param<bool>("publish/scan_publish_en",scan_pub_en, true);                    // 是否发布当前正在扫描的点云的topic
    nh.param<bool>("publish/dense_publish_en",dense_pub_en, true);                  // 是否发布经过运动畸变校正注册到IMU坐标系的点云的topic，
    nh.param<bool>("publish/scan_bodyframe_pub_en",scan_body_pub_en, true);         // 是否发布经过运动畸变校正注册到IMU坐标系的点云的topic，需要该变量和上一个变量同时为true才发布
    nh.param<int>("max_iteration",NUM_MAX_ITERATIONS,4);                            // 卡尔曼滤波的最大迭代次数
    nh.param<string>("map_file_path",map_file_path,"");                             // 地图保存路径
    nh.param<string>("common/lid_topic",lid_topic,"/velodyne_points");                  // 激光雷达点云topic名称
    nh.param<string>("common/imu_topic", imu_topic,"/imu/data");                   // IMU的topic名称
    nh.param<bool>("common/time_sync_en", time_sync_en, false);                     // 是否需要时间同步，只有当外部未进行时间同步时设为true
    nh.param<double>("filter_size_corner",filter_size_corner_min,0.5);              // VoxelGrid降采样时的体素大小
    nh.param<double>("filter_size_surf",filter_size_surf_min,0.5);                  // VoxelGrid降采样时的体素大小
    nh.param<double>("filter_size_map",filter_size_map_min,0.5);                    // VoxelGrid降采样时的体素大小
    nh.param<double>("cube_side_length",cube_len,200);                              // 地图的局部区域的长度（FastLio2论文中有解释）
    nh.param<float>("mapping/det_range",DET_RANGE,50.f);                           // 激光雷达的最大探测范围 300 只是用来判断是否挪动地图
    nh.param<double>("mapping/fov_degree",fov_deg,180);                             // 激光雷达的视场角 180
    nh.param<double>("mapping/gyr_cov",gyr_cov,0.1);                                // IMU陀螺仪的协方差
    nh.param<double>("mapping/acc_cov",acc_cov,0.1);                                // IMU加速度计的协方差
    nh.param<double>("mapping/b_gyr_cov",b_gyr_cov,0.0001);                         // IMU陀螺仪偏置的协方差
    nh.param<double>("mapping/b_acc_cov",b_acc_cov,0.0001);                         // IMU加速度计偏置的协方差
    nh.param<double>("preprocess/blind", p_pre->blind, 0.01);                       // 最小距离阈值，即过滤掉0～blind范围内的点云
    nh.param<int>("preprocess/lidar_type", p_pre->lidar_type, AVIA);                // 激光雷达的类型
    nh.param<int>("preprocess/scan_line", p_pre->N_SCANS, 16);                      // 激光雷达扫描的线数（livox avia为6线）
    nh.param<int>("preprocess/scan_rate", p_pre->SCAN_RATE, 10);                    // 激光雷达的扫描频率（livox avia为10hz）
    nh.param<int>("point_filter_num", p_pre->point_filter_num, 2);                  // 采样间隔，即每隔point_filter_num个点取1个点
    nh.param<bool>("feature_extract_enable", p_pre->feature_enabled, false);        // 是否提取特征点（FAST_LIO2默认不进行特征点提取）
    nh.param<bool>("runtime_pos_log_enable", runtime_pos_log, 0);                   // 是否输出调试log信息
    nh.param<bool>("mapping/extrinsic_est_en", extrinsic_est_en, true);             // 是否进行在线的外参估计
    nh.param<bool>("pcd_save/pcd_save_en", pcd_save_en, false);                     // 是否将点云地图保存到PCD文件
    nh.param<int>("pcd_save/interval", pcd_save_interval, -1);                      // 每一个PCD文件保存多少个雷达帧（-1表示所有雷达帧都保存在一个PCD文件中）
    nh.param<vector<double>>("mapping/extrinsic_T", extrinT, vector<double>());     // 雷达相对于IMU的外参T（即雷达在IMU坐标系中的坐标）
    nh.param<vector<double>>("mapping/extrinsic_R", extrinR, vector<double>());     // 雷达相对于IMU的外参R
    cout<<"p_pre->lidar_type "<<p_pre->lidar_type<<endl;                            // 打印雷达类型
    
    // 初始化path的header（包括时间戳和帧id），path用于保存odemetry的路径
    path.header.stamp    = ros::Time::now();
    path.header.frame_id ="camera_init";

    /*** variables definition ***/
    /** 变量定义
     * effect_feat_num          （后面的代码中没有用到该变量）
     * frame_num                雷达总帧数
     * deltaT                   （后面的代码中没有用到该变量）
     * deltaR                   （后面的代码中没有用到该变量）
     * 
     * aver_time_consu          每帧平均的处理总时间
     * aver_time_icp            每帧中icp的平均时间
     * aver_time_match          每帧中匹配的平均时间
     * aver_time_incre          每帧中ikd-tree增量处理的平均时间
     * aver_time_solve          每帧中计算的平均时间
     * aver_time_const_H_time   每帧中计算的平均时间（当H恒定时）
     * 
     * flg_EKF_converged        （后面的代码中没有用到该变量）EKF收敛标志
     * EKF_stop_flg             （后面的代码中没有用到该变量）EKF停止标志
     * FOV_DEG                  （后面的代码中没有用到该变量）视场角
     * HALF_FOV_COS             （后面的代码中没有用到该变量）视场角一半余弦值
     * _featsArray              （后面的代码中没有用到该变量）
     */
    int effect_feat_num = 0, frame_num = 0;
    double deltaT, deltaR, aver_time_consu = 0, aver_time_icp = 0, aver_time_match = 0, aver_time_incre = 0, aver_time_solve = 0, aver_time_const_H_time = 0;
    bool flg_EKF_converged, EKF_stop_flg = 0;
    FOV_DEG = (fov_deg + 10.0) > 179.9 ? 179.9 : (fov_deg + 10.0);
    HALF_FOV_COS = cos((FOV_DEG) * 0.5 * PI_M / 180.0);
    _featsArray.reset(new PointCloudXYZI());//智能指针初始化
    
    // 将数组point_selected_surf内元素的值全部设为true，数组point_selected_surf用于选择存储面点
    memset(point_selected_surf, true, sizeof(point_selected_surf));
    // 将数组res_last内元素的值全部设置为-1000.0f，数组res_last用于
    memset(res_last, -1000.0f, sizeof(res_last));

    // VoxelGrid滤波器参数，即进行滤波时的创建的体素边长为filter_size_surf_min，xyz
    downSizeFilterSurf.setLeafSize(filter_size_surf_min, filter_size_surf_min, filter_size_surf_min);
    // VoxelGrid滤波器参数，即进行滤波时的创建的体素边长为filter_size_map_min
    downSizeFilterMap.setLeafSize(filter_size_map_min, filter_size_map_min, filter_size_map_min);

    // 感觉跟上面重复了
    memset(point_selected_surf, true, sizeof(point_selected_surf));
    memset(res_last, -1000.0f, sizeof(res_last));

    // 输出雷达相对于IMU的外参R和T
    Lidar_T_wrt_IMU<<VEC_FROM_ARRAY(extrinT);//从3个0.0的数组创建一个向量，初始化是0 ，后从外传参赋值
    Lidar_R_wrt_IMU<<MAT_FROM_ARRAY(extrinR);

    // 设置IMU的参数，对p_imu进行初始化，其中p_imu为ImuProcess的智能指针（ImuProcess是进行IMU处理的类）
    p_imu->set_extrinsic(Lidar_T_wrt_IMU, Lidar_R_wrt_IMU);//设置外参，imu转化到雷达坐标系
    p_imu->set_gyr_cov(V3D(gyr_cov, gyr_cov, gyr_cov));//设置陀螺仪随机误差
    p_imu->set_acc_cov(V3D(acc_cov, acc_cov, acc_cov));//设置加速度计随机误差
    p_imu->set_gyr_bias_cov(V3D(b_gyr_cov, b_gyr_cov, b_gyr_cov));//设置陀螺仪偏置
    p_imu->set_acc_bias_cov(V3D(b_acc_cov, b_acc_cov, b_acc_cov));//设置陀螺仪偏置

    // TODOesiekf设置
    // 按照单元赋值，将一个区间的元素都赋同一个值
    // fill(arr, arr + 维数, 要填入的内容);
    double epsi[23] = {0.001};
    fill(epsi, epsi+23, 0.001);
    //接收特定于系统的模型及其差异
    //作为一个维数变化的特征矩阵进行测量。
    //通过一个函数（h_dyn_share_in）同时计算观测值（z）、预测值（h）、偏微分矩阵（h_x，h_v）和噪声协方差（R）。
    kf.init_dyn_share(get_f, df_dx, df_dw, h_share_model, NUM_MAX_ITERATIONS, epsi);//TODO传播模型,传播参数1,传播参数2,观测模型,最大迭代次数，每个维度的差异？

    /*** debug record ***/
    // 将调试log输出到文件中
    FILE *fp;
    string pos_log_dir = root_dir + "/Log/pos_log.txt";
    fp = fopen(pos_log_dir.c_str(),"w");
    ofstream fout_pre, fout_out, fout_dbg;
    fout_pre.open(DEBUG_FILE_DIR("mat_pre.txt"),ios::out);
    fout_out.open(DEBUG_FILE_DIR("mat_out.txt"),ios::out);
    fout_dbg.open(DEBUG_FILE_DIR("dbg.txt"),ios::out);
    if (fout_pre && fout_out)
        cout << "~~~~"<<ROOT_DIR<<" file opened" << endl;
    else
        cout << "~~~~"<<ROOT_DIR<<" doesn't exist" << endl;

    
    // ROS订阅器和发布器的定义和初始化***********************************************************************************
    /*** ROS subscribe initialization ***/
     // 雷达点云的订阅器sub_pcl，订阅点云的topic，预处理存入缓存区
    ros::Subscriber sub_pcl = p_pre->lidar_type == AVIA ? \
        nh.subscribe(lid_topic, 200000, standard_pcl_cbk) : \
        nh.subscribe(lid_topic, 200000, standard_pcl_cbk);
    // IMU的订阅器sub_imu，订阅IMU的topic，直接存入缓存区
    ros::Subscriber sub_imu = nh.subscribe(imu_topic, 200000, imu_cbk);
   
     /*** ROS Publisher initialization ***/
    // 发布当前正在扫描的点云转化到世界坐标系下的点云，topic名字为/cloud_registered,对应surround以及currpoints（延时了100000，延时之后相等）
    ros::Publisher pubLaserCloudFull = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_registered", 100000);
    // 发布经过运动畸变校正注册到IMU坐标系的点云，topic名字为/cloud_registered_body
    ros::Publisher pubLaserCloudFull_body = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_registered_body", 100000);
    // 后面的代码中没有用到
    ros::Publisher pubLaserCloudEffect = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_effected", 100000);
    // 后面的代码中没有用到，对应pointcloud2,没有效果
    ros::Publisher pubLaserCloudMap = nh.advertise<sensor_msgs::PointCloud2>
            ("/Laser_map", 100000);
    // 发布当前里程计信息，topic名字为/Odometry
    ros::Publisher pubOdomAftMapped = nh.advertise<nav_msgs::Odometry> 
            ("/lio", 100000);
    // 发布里程计总的路径，topic名字为/path
    ros::Publisher pubPath          = nh.advertise<nav_msgs::Path> 
            ("/path", 100000);

//------------------------------------------------------------------------------------------------------

    // 中断处理函数，如果有中断信号（比如Ctrl+C），则执行第二个参数里面的SigHandle函数
    signal(SIGINT, SigHandle);

    // 设置ROS程序主循环每次运行的时间至少为0.0002秒（5000Hz）
    ros::Rate rate(5000);
    bool status = ros::ok();
    // 程序主循环
    while (status)
    {
        // 如果有中断产生，则结束主循环
        if (flg_exit) break;

        // ROS消息回调处理函数，放在ROS的主循环中
        ros::spinOnce();

        // 将激光雷达点云数据和IMU数据从缓存队列中取出，进行时间对齐，并保存到Measures中
        if(sync_packages(Measures)) 
        {
            // 激光雷达第一次扫描
            if (flg_first_scan)
            {
                // 记录激光雷达第一次扫描的时间
                first_lidar_time = Measures.lidar_beg_time;
                p_imu->first_lidar_time = first_lidar_time;
                flg_first_scan = false;
                continue;
            }

            double t0,t1,t2,t3,t4,t5,match_start, solve_start, svd_time;

            match_time = 0;
            kdtree_search_time = 0.0;
            solve_time = 0;
            solve_const_H_time = 0;
            svd_time   = 0;
            t0 = omp_get_wtime();//imu预处理前开始时间

            //TODO 对IMU数据进行预处理，其中包含了点云畸变处理，前向传播 反向传播
            p_imu->Process(Measures, kf, feats_undistort);//feats 处理后的数据

            //世界系下雷达坐标系的位置
            //下面式子的意义是W^p_L = W^p_I + W^R_I * I^t_L
            state_point = kf.get_x();//TODO
            pos_lid = state_point.pos + state_point.rot * state_point.offset_T_L_I;//TODO

            if (feats_undistort->empty() || (feats_undistort == NULL))//如果处理后的数据为空报错
            {
                ROS_WARN("No point, skip this scan!\n");
                continue;
            }

            flg_EKF_inited = (Measures.lidar_beg_time - first_lidar_time) < INIT_TIME ? false : true;//初始化时间 0.1
            
            /*** Segment the map in lidar FOV 在激光雷达视场中分割地图***/
            lasermap_fov_segment();

            /*** downsample the feature points in a scan ***/
            downSizeFilterSurf.setInputCloud(feats_undistort);
            downSizeFilterSurf.filter(*feats_down_body);
            t1 = omp_get_wtime();//降采样后的时间
            feats_down_size = feats_down_body->points.size();
            
            /*** initialize the map kdtree ***/
            if(ikdtree.Root_Node == nullptr)            //初始化ikdtree
            {
                if(feats_down_size > 5)
                {
                    ikdtree.set_downsample_param(filter_size_map_min);
                    feats_down_world->resize(feats_down_size);
                    for(int i = 0; i < feats_down_size; i++)
                    {
                        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
                    }
                    ikdtree.Build(feats_down_world->points);//构建世界地图下的ikdtree
                }
                continue;
            }
            int featsFromMapNum = ikdtree.validnum();
            kdtree_size_st = ikdtree.size();
            
            // cout<<"[ mapping ]: In num: "<<feats_undistort->points.size()<<" downsamp "<<feats_down_size<<" Map num: "<<featsFromMapNum<<"effect num:"<<effct_feat_num<<endl;

            /*** ICP and iterated Kalman filter update ***/
            if (feats_down_size < 5)//点太少就不处理继续
            {
                ROS_WARN("No point, skip this scan!\n");
                continue;
            }
            
            normvec->resize(feats_down_size);
            feats_down_world->resize(feats_down_size);

            V3D ext_euler = SO3ToEuler(state_point.offset_R_L_I);
            fout_pre<<setw(20)<<Measures.lidar_beg_time - first_lidar_time<<" "<<euler_cur.transpose()<<" "<< state_point.pos.transpose()<<" "<<ext_euler.transpose() << " "<<state_point.offset_T_L_I.transpose()<< " " << state_point.vel.transpose() \
            <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<< endl;

            if(1) // If you need to see map point, change to "if(1)"
            {
                PointVector ().swap(ikdtree.PCL_Storage);
                ikdtree.flatten(ikdtree.Root_Node, ikdtree.PCL_Storage, NOT_RECORD);
                featsFromMap->clear();
                featsFromMap->points = ikdtree.PCL_Storage;
            }

            pointSearchInd_surf.resize(feats_down_size);
            Nearest_Points.resize(feats_down_size);
            int  rematch_num = 0;
            bool nearest_search_en = true; //

            t2 = omp_get_wtime();
            
            /*** iterated state estimation ***/
            double t_update_start = omp_get_wtime();
            double solve_H_time = 0;
            //迭代卡尔曼滤波更新
            kf.update_iterated_dyn_share_modified(LASER_POINT_COV, solve_H_time);
            state_point = kf.get_x();
            euler_cur = SO3ToEuler(state_point.rot);
            pos_lid = state_point.pos + state_point.rot * state_point.offset_T_L_I;
            geoQuat.x = state_point.rot.coeffs()[0];
            geoQuat.y = state_point.rot.coeffs()[1];
            geoQuat.z = state_point.rot.coeffs()[2];
            geoQuat.w = state_point.rot.coeffs()[3];

            double t_update_end = omp_get_wtime();

            /******* Publish odometry *******/
            publish_odometry(pubOdomAftMapped);

            /*** add the feature points to map kdtree ***/
            t3 = omp_get_wtime();//构建kdtree前时间
            map_incremental();
            t5 = omp_get_wtime();//构建后时间
            
            /******* Publish points *******/
            if (path_en)                         publish_path(pubPath);//发布路径
            if (scan_pub_en || pcd_save_en)      publish_frame_world(pubLaserCloudFull);//发布当前扫描的点云转化到世界坐标系下，累积就是地图
            if (scan_pub_en && scan_body_pub_en) publish_frame_body(pubLaserCloudFull_body);//发布当前扫描的点云转化到imu坐标系下。同上，两个效果基本一样
            publish_effect_world(pubLaserCloudEffect);
            publish_map(pubLaserCloudMap);//对应pointcloud2，无用，打开上面if（0）就能看到，但是是看的什么？

            /*** Debug variables ***/
            if (runtime_pos_log)
            {
                frame_num ++;
                kdtree_size_end = ikdtree.size();
                aver_time_consu = aver_time_consu * (frame_num - 1) / frame_num + (t5 - t0) / frame_num;
                aver_time_icp = aver_time_icp * (frame_num - 1)/frame_num + (t_update_end - t_update_start) / frame_num;
                aver_time_match = aver_time_match * (frame_num - 1)/frame_num + (match_time)/frame_num;
                aver_time_incre = aver_time_incre * (frame_num - 1)/frame_num + (kdtree_incremental_time)/frame_num;
                aver_time_solve = aver_time_solve * (frame_num - 1)/frame_num + (solve_time + solve_H_time)/frame_num;
                aver_time_const_H_time = aver_time_const_H_time * (frame_num - 1)/frame_num + solve_time / frame_num;
                T1[time_log_counter] = Measures.lidar_beg_time;
                s_plot[time_log_counter] = t5 - t0;
                s_plot2[time_log_counter] = feats_undistort->points.size();
                s_plot3[time_log_counter] = kdtree_incremental_time;
                s_plot4[time_log_counter] = kdtree_search_time;
                s_plot5[time_log_counter] = kdtree_delete_counter;
                s_plot6[time_log_counter] = kdtree_delete_time;
                s_plot7[time_log_counter] = kdtree_size_st;
                s_plot8[time_log_counter] = kdtree_size_end;
                s_plot9[time_log_counter] = aver_time_consu;
                s_plot10[time_log_counter] = add_point_size;
                time_log_counter ++;
                printf("[ mapping ]: time: IMU + Map + Input Downsample: %0.6f ave match: %0.6f ave solve: %0.6f  ave ICP: %0.6f  map incre: %0.6f ave total: %0.6f icp: %0.6f construct H: %0.6f \n",t1-t0,aver_time_match,aver_time_solve,t3-t1,t5-t3,aver_time_consu,aver_time_icp, aver_time_const_H_time);
                ext_euler = SO3ToEuler(state_point.offset_R_L_I);
                fout_out << setw(20) << Measures.lidar_beg_time - first_lidar_time << " " << euler_cur.transpose() << " " << state_point.pos.transpose()<< " " << ext_euler.transpose() << " "<<state_point.offset_T_L_I.transpose()<<" "<< state_point.vel.transpose() \
                <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<<" "<<feats_undistort->points.size()<<endl;
                dump_lio_state_to_log(fp);
            }
        }

        status = ros::ok();
        rate.sleep();
    }

    /**************** save map ****************/
    /* 1. make sure you have enough memories
    /* 2. pcd save will largely influence the real-time performences **/
    if (pcl_wait_save->size() > 0 && pcd_save_en)
    {
        string file_name = string("scans.pcd");
        string all_points_dir(string(string(ROOT_DIR) + "PCD/") + file_name);//pcd文件名
        pcl::PCDWriter pcd_writer;
        cout << "current scan saved to /PCD/" << file_name<<endl;
        pcd_writer.writeBinary(all_points_dir, *pcl_wait_save);//保存pcd地图函数
    }

    fout_out.close();
    fout_pre.close();
    //保存日志
    if (runtime_pos_log)
    {
        vector<double> t, s_vec, s_vec2, s_vec3, s_vec4, s_vec5, s_vec6, s_vec7;    
        FILE *fp2;
        string log_dir = root_dir + "/Log/fast_lio_time_log.csv";
        fp2 = fopen(log_dir.c_str(),"w");
        fprintf(fp2,"time_stamp, total time, scan point size, incremental time, search time, delete size, delete time, tree size st, tree size end, add point size, preprocess time\n");
        for (int i = 0;i<time_log_counter; i++){
            fprintf(fp2,"%0.8f,%0.8f,%d,%0.8f,%0.8f,%d,%0.8f,%d,%d,%d,%0.8f\n",T1[i],s_plot[i],int(s_plot2[i]),s_plot3[i],s_plot4[i],int(s_plot5[i]),s_plot6[i],int(s_plot7[i]),int(s_plot8[i]), int(s_plot10[i]), s_plot11[i]);
            t.push_back(T1[i]);
            s_vec.push_back(s_plot9[i]);
            s_vec2.push_back(s_plot3[i] + s_plot6[i]);
            s_vec3.push_back(s_plot4[i]);
            s_vec5.push_back(s_plot[i]);
        }
        fclose(fp2);
    }

    return 0;
}
