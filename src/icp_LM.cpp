//
// Created by julian on 29.03.22.
//

#include "icp_LM.h"
#include "utils.h"

#include <fmt/printf.h>
#include <fmt/ostream.h>
#include <pcl/common/transforms.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/features/normal_3d_omp.h>

#include <ceres/ceres.h>

ICP_LM::ICP_LM(float grid_size, float distance_threshold, int iterations)
: _distance_threshold(distance_threshold)
, _iterations(iterations)
, _state(State::Zero())
, _model(new pcl::PointCloud<pcl::PointXYZ>)
, _scene(new pcl::PointCloud<pcl::PointXYZ>)
, _voxel_filter()
{
    _voxel_filter.setLeafSize(grid_size, grid_size, grid_size);
}

void ICP_LM::set_model(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr &model) const
{
    pcl::copyPointCloud(*model, *_model);
}

Eigen::Matrix4f ICP_LM::align(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr &scene)
{
    pcl::copyPointCloud(*scene, *_scene);
    StopWatch time, total_time;
    
    _voxel_filter.setInputCloud(_model);
    _voxel_filter.filter(*_model);
    
    // voxel filter (scene)
    _voxel_filter.setInputCloud(_scene);
    _voxel_filter.filter(*_scene);

    // calculate normals
    pcl::NormalEstimationOMP<pcl::PointXYZ, pcl::Normal> ne;
    pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
    // tree is empty now but will be filled after normal calculation
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ> ());
    ne.setInputCloud(_model);
    ne.setSearchMethod(tree);
    ne.setKSearch(10);
    ne.compute(*normals);

    float lambda = 1e-2;
    
    fmt::print("preprocessing took {}ms\n", time.stop<std::chrono::milliseconds>());
    Eigen::Matrix3f R = Eigen::Matrix3f::Identity();
    Eigen::Vector3f t = Eigen::Vector3f::Zero();
    std::tie(R, t) = state_2_rot_trans(_state);
    
    for (int j = 0; j < _iterations; j++)
    {
        time.reset();
        // for every point in scene, get closest point in model
        std::vector<int> indices(_scene->size());
        std::iota(indices.begin(), indices.end(), 0);
        std::vector<std::vector<int>> correspondences(_scene->size());
        std::vector<std::vector<float>> point_squared_distance(_scene->size());
        tree->nearestKSearch(*_scene, indices, 1, correspondences, point_squared_distance);

        Hessian H = Hessian::Zero();
        State gradient = State::Zero();

        auto [ R_hat, t_hat ] = state_2_rot_trans(_state);
        float chi = 0.f;

        // correspondences (nearest neighbor, normals) are calculated
        for (int i = 0; i < _scene->size(); ++i)
        {
            const Eigen::Vector3f &scene_point = ((*_scene)[i]).getVector3fMap();
            const Eigen::Vector3f &corr_point = ((*_model)[correspondences[i][0]]).getVector3fMap();
            const Eigen::Vector3f &corr_normal = ((*normals)[correspondences[i][0]]).getNormalVector3fMap();
            
            // rotate scene point with current best guess
            auto rot_scene_point = R_hat * scene_point;
            float error = (rot_scene_point + t_hat - corr_point).dot(corr_normal);

            Eigen::Vector6f J = jacobian_plane(rot_scene_point, corr_normal);
            H += J.transpose() * J;
            gradient += J.transpose() * error;
            chi += std::abs(error);
        }

        H += lambda * H * Hessian::Identity();
        State update = -H.inverse() * gradient;
        State state_new = _state + update;
        
        auto [ R_new, t_new ] = state_2_rot_trans(state_new);
        float chi_new = 0;
        
        // recalculate error
        for (int i = 0; i < _scene->size(); ++i)
        {
            const Eigen::Vector3f &scene_point = ((*_scene)[i]).getVector3fMap();
            const Eigen::Vector3f &corr_point = ((*_model)[correspondences[i][0]]).getVector3fMap();
            const Eigen::Vector3f &corr_normal = ((*normals)[correspondences[i][0]]).getNormalVector3fMap();
            
            auto rot_scene_point_new = R_new * scene_point;
            float error_new = (rot_scene_point_new + t_new - corr_point).dot(corr_normal);
            chi_new += std::abs(error_new);
        }

        if (chi_new > chi)
        {
            lambda *= 10;
        }
        else
        {
            _state = state_new;
            lambda /= 10;
        }
    
        std::tie(R_hat, t_hat) = state_2_rot_trans(_state);
    
        Eigen::Matrix4f pcl_transform = Eigen::Matrix4f::Identity();
        pcl_transform.block<3, 3>(0, 0) = R_hat;
        pcl_transform.block<3, 1>(0, 3) = t_hat;
    
        R = R_hat * R;
        t = R_hat * t + t_hat;
    
        // update pcl for next iteration
        pcl::transformPointCloud(*_scene, *_scene, pcl_transform);
        
        fmt::print("it: {}, err: {} in {}ms\n", j, chi / (float)correspondences.size(), time.stop<std::chrono::milliseconds>());
    }
    fmt::print("total processing time {}ms\n", total_time.stop<std::chrono::milliseconds>());
    
    Eigen::Matrix4f final_transform = Eigen::Matrix4f::Identity();
    final_transform.block<3, 3>(0, 0) = R;
    final_transform.block<3, 1>(0, 3) = t;
    return final_transform;
}

Eigen::Matrix4f ICP_LM::align_ceres(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr &scene)
{
    pcl::copyPointCloud(*scene, *_scene);
    StopWatch time, total_time;

    _voxel_filter.setInputCloud(_model);
    _voxel_filter.filter(*_model);

    // voxel filter (scene)
    _voxel_filter.setInputCloud(_scene);
    _voxel_filter.filter(*_scene);

    // calculate normals
    pcl::NormalEstimationOMP<pcl::PointXYZ, pcl::Normal> ne;
    pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
    // tree is empty now but will be filled after normal calculation
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ> ());
    ne.setInputCloud(_model);
    ne.setSearchMethod(tree);
    ne.setKSearch(20);
    ne.compute(*normals);

    fmt::print("preprocessing took {}ms\n", time.stop<std::chrono::milliseconds>());

    Eigen::Quaterniond q_w_curr(1, 0, 0, 0);
    Eigen::Vector3d t_w_curr(0, 0, 0);
    double para_q[4] = {0, 0, 0, 1};
    double para_t[3] = {0, 0, 0};
    Eigen::Map<Eigen::Quaterniond> q_last_curr(para_q);
    Eigen::Map<Eigen::Vector3d> t_last_curr(para_t);

    size_t skips = 0;

    for (int j = 0; j < _iterations; j++)
    {
        ceres::LossFunction *loss_function = new ceres::HuberLoss(0.3);
        ceres::LocalParameterization *q_parameterization =
                new ceres::EigenQuaternionParameterization();
        ceres::Problem::Options problem_options;

        ceres::Problem problem(problem_options);
        problem.AddParameterBlock(para_q, 4, q_parameterization);
        problem.AddParameterBlock(para_t, 3);

        time.reset();
        // for every point in scene, get closest point in model
        std::vector<int> indices(_scene->size());
        std::iota(indices.begin(), indices.end(), 0);
        std::vector<std::vector<int>> correspondences(_scene->size());
        std::vector<std::vector<float>> point_squared_distance(_scene->size());
        tree->nearestKSearch(*_scene, indices, 1, correspondences, point_squared_distance);

        // correspondences (nearest neighbor, normals) are calculated
        for (int i = 0; i < _scene->size(); ++i)
        {
            const Eigen::Vector3f &scene_point = ((*_scene)[i]).getVector3fMap();
            const Eigen::Vector3f &corr_point = ((*_model)[correspondences[i][0]]).getVector3fMap();
            const Eigen::Vector3f &corr_normal = ((*normals)[correspondences[i][0]]).getNormalVector3fMap();

            if (correspondences[i][0] < _scene->size())
            {
                if (point_squared_distance[correspondences[i][0]][0] > _distance_threshold)
                {
                    ++skips;
                    continue;
                }
            }

            ceres::CostFunction *cost_function = Point2PlaneFactor::Create(scene_point.cast<double>(), corr_point.cast<double>(), corr_normal.cast<double>());
            problem.AddResidualBlock(cost_function, loss_function, para_q, para_t);
        }

        StopWatch t_solver;
        ceres::Solver::Options options;
        options.linear_solver_type = ceres::DENSE_QR;
        options.max_num_iterations = 4;
        options.minimizer_progress_to_stdout = false;
        options.check_gradients = false;
        options.gradient_check_relative_precision = 1e-4;
        ceres::Solver::Summary summary;
        ceres::Solve(options, &problem, &summary);
        printf("solver time %f ms \n", t_solver.stop<std::chrono::milliseconds>());
    }

    t_w_curr = t_w_curr + q_w_curr * t_last_curr;
    q_w_curr = q_w_curr * q_last_curr;
    fmt::print("total processing time {}ms with {} skips\n", total_time.stop<std::chrono::milliseconds>(), skips);

    Eigen::Matrix3f rotm = q_w_curr.toRotationMatrix().cast<float>();

    Eigen::Matrix4f final_transform = Eigen::Matrix4f::Identity();
    final_transform.block<3, 3>(0, 0) = rotm;
    final_transform.block<3, 1>(0, 3) = t_w_curr.cast<float>();
    return final_transform;
}