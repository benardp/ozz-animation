//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2017 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/ik_aim_job.h"
#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"

#include "ozz/base/log.h"

#include "ozz/base/maths/box.h"
#include "ozz/base/maths/math_ex.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"

#include "ozz/options/options.h"

#include "framework/application.h"
#include "framework/imgui.h"
#include "framework/mesh.h"
#include "framework/utils.h"

#include "framework/renderer.h"
#include "framework/utils.h"

#include <limits>

// Skeleton archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(skeleton,
                           "Path to the skeleton (ozz archive format).",
                           "media/skeleton.ozz", false)

// Animation archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(animation,
                           "Path to the animation (ozz archive format).",
                           "media/animation.ozz", false)

// Mesh archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(mesh,
                           "Path to the skinned mesh (ozz archive format).",
                           "media/mesh.ozz", false)

// Mesh archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(floor,
                           "Path to the floor mesh (ozz archive format).",
                           "media/floor.ozz", false)

// Structure used to store each leg setup data.
struct LegSetup {
  int hip;
  int knee;
  int ankle;
};

struct LegRayInfo {
  ozz::math::Float3 start;
  ozz::math::Float3 dir;

  bool hit;
  ozz::math::Float3 hit_point;
  ozz::math::Float3 hit_normal;
};

// TODO
const ozz::math::Float3 down(0.f, -1.f, 0.f);
const ozz::math::Float3 character_height_offset(0.f, 10.f, 0.f);
const ozz::math::Float3 foot_height_offset(0.f, .5f, 0.f);

class FootIKSampleApplication : public ozz::sample::Application {
 public:
  FootIKSampleApplication()
      : pelvis_offset_(0.f, 0.f, 0.f),
        root_translation_(5.1f, 2.f, -2.1f),
        root_yaw_(1.f),
        foot_heigh_(.12f),
        weight_(1.f),
        soften_(1.f),
        auto_character_height_(true),
        pelvis_correction_(true),
        two_bone_ik_(true),
        aim_ik_(true),
        show_skin_(true),
        show_joints_(false),
        show_raycast_(false),
        show_ankle_target_(false),
        show_root_(false),
        show_offseted_root_(false) {}

 protected:
  // Updates current animation time and foot ik.
  virtual bool OnUpdate(float _dt, float) {
    const ozz::math::Float4x4 root = GetRootTransform();

    // TODO
    if (!UpdateCharacterHeight()) {
      return false;
    }

    if (!UpdateBaseAnimation(_dt)) {
      return false;
    }

    // Raycast down from the ankle to the floor.
    if (!RaycastLegs(root)) {
      return false;
    }

    // TODO
    if (!UpdateAnklesTarget()) {
      return false;
    }

    if (!UpdatePelvisOffset()) {
      return false;
    }

    if (!UpdateFootIK(root)) {
      return false;
    }

    return true;
  }

  bool UpdateCharacterHeight() {
    if (!auto_character_height_) {
      return true;
    }

    // Raycast down from the current position to find character height.
    // Updates root translation as output.
    ozz::sample::RayIntersectsMeshes(
        root_translation_ + character_height_offset, down, make_range(floors_),
        &root_translation_, NULL);

    return true;
  }

  bool UpdateBaseAnimation(float _dt) {
    // Updates current animation time.
    controller_.Update(animation_, _dt);

    // Samples optimized animation at t = animation_time.
    ozz::animation::SamplingJob sampling_job;
    sampling_job.animation = &animation_;
    sampling_job.cache = &cache_;
    sampling_job.ratio = controller_.time_ratio();
    sampling_job.output = make_range(locals_);
    if (!sampling_job.Run()) {
      return false;
    }

    // Converts from local space to model space matrices.
    ozz::animation::LocalToModelJob ltm_job;
    ltm_job.skeleton = &skeleton_;
    ltm_job.input = make_range(locals_);
    ltm_job.output = make_range(models_);
    if (!ltm_job.Run()) {
      return false;
    }
    return true;
  }

  bool RaycastLegs(const ozz::math::Float4x4& _root) {
    // Raycast down for each leg to find the intersection point with the floor.
    for (size_t l = 0; l < kLegsCount; ++l) {
      const LegSetup& leg = legs_setup_[l];
      LegRayInfo& ray = rays_info_[l];

      // Finds ankle world space position
      // Updates at the same time ankles_target_ws_
      ozz::math::Store3PtrU(TransformPoint(_root, models_[leg.ankle].cols[3]),
                            &ankles_target_ws_[l].x);

      // Builds ray, from ankle and going downward.
      ray.start = ankles_target_ws_[l] + foot_height_offset;
      ray.dir = down;
      ray.hit = ozz::sample::RayIntersectsMeshes(
          ray.start, ray.dir, make_range(floors_), &ray.hit_point,
          &ray.hit_normal);
    }

    return true;
  }

  // Comptutes ankle target position, so that the foot is in contact with the
  // floor. This needs to consider slope angle (floor normal) and foot height.
  // See geogebra diagram for more details: media/doc/samples/sample_foot_ik_ankle.ggb
  bool UpdateAnklesTarget() {
    for (size_t l = 0; l < kLegsCount; ++l) {
      const LegRayInfo& ray = rays_info_[l];
      if (!ray.hit) {
        continue;
      }

      // Computes projection of the ray AI (from start to floor intersection
      // point) onto floor normal. This gives the length of segment AB.
      // Note that ray.hit_normal is normalized already.
      const float ABl = Dot(ray.start - ray.hit_point, ray.hit_normal);
      if (ABl == 0.f) {
        // Early out if the two are perpandicular.
        continue;
      }

      // Knowing A, AB length and direction, we can compute B position.
      const ozz::math::Float3 B = ray.start - ray.hit_normal * ABl;

      // Computes sebgment IB and its length (IBl)
      const ozz::math::Float3 IB = B - ray.hit_point;
      const float IBl = Length(IB);

      if (IBl <= 0.f) {
        // If B is at raycast intersection (I), then we still need to update
        // corrected ankle position (world-space) to take into account foot
        // height.
        ankles_target_ws_[l] = ray.hit_point + ray.hit_normal * foot_heigh_;
      } else {
        // HC length is known (as foot height). So we're using Thales theorem to
        // find H position.
        const float IHl = IBl * foot_heigh_ / ABl;
        const ozz::math::Float3 IH = IB * (IHl / IBl);
        const ozz::math::Float3 H = ray.hit_point + IH;

        // C (Corrected ankle) position can now be found.
        const ozz::math::Float3 C = H + ray.hit_normal * foot_heigh_;

        // Override ankle position with result.
        ankles_target_ws_[l] = C;
      }
    }

    return true;
  }

  // Recomputes pelvis offset.
  // Strategy is to move the pelvis along "down" axis (ray axis), enough for
  // the lowest foot (lowest from its original position) to touch the floor.
  // The other foot will be ik-ed.
  bool UpdatePelvisOffset() {
    pelvis_offset_ = ozz::math::Float3(0.f, 0.f, 0.f);

    float max_dot = -std::numeric_limits<float>::max();
    if (pelvis_correction_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const LegRayInfo& ray = rays_info_[l];
        if (!ray.hit) {
          continue;
        }

        const ozz::math::Float3 ankle = ray.start - foot_height_offset;  // TODO
        const ozz::math::Float3 ankle_to_target = ankles_target_ws_[l] - ankle;

        // Check if this ankle is lower (in down direction) compared to the
        // previous one.
        const float dot = Dot(ankle_to_target, down);
        if (dot > max_dot) {
          max_dot = dot;

          // Compute offset using the maximum displacement that the legs should
          // have to touch ground.
          pelvis_offset_ = down * dot;
        }
      }
    }

    return true;
  }

  // Applies two bone IK to the leg, and aim IK to the ankle
  bool UpdateFootIK(const ozz::math::Float4x4& _root) {
    const ozz::math::Float4x4 inv_root = Invert(_root);

    ozz::animation::LocalToModelJob ltm_job;
    ltm_job.skeleton = &skeleton_;
    ltm_job.input = make_range(locals_);
    ltm_job.output = make_range(models_);

    // Perform IK
    for (size_t l = 0; l < kLegsCount; ++l) {
      const LegRayInfo& ray = rays_info_[l];
      if (!ray.hit) {
        continue;
      }
      const LegSetup& leg = legs_setup_[l];

      /*
      // Computes
  // TODO target was computed before pelvis is offseted


  // Computes
  // TODO target was computed before pelvis is offseted
          // Ankle target positions needs to be corrected with the correction
applied
// to pelvis.

  ankles_target_ws_[l] = ankles_target_ws_[l] - pelvis_offset_;*/
      const ozz::math::Float3 target(ankles_target_ws_[l] - pelvis_offset_);

      if (two_bone_ik_ && !ApplyLegTwoBoneIK(leg, target, inv_root)) {
        return false;
      }

      // Updates leg joints model-space transforms.
      // Update will go from hip to ankle. Ankle's siblings might no be updated
      // as local-to-model will stop as soon as ankle joint is reached.
      ltm_job.from = leg.hip;
      ltm_job.to = leg.ankle;
      if (!ltm_job.Run()) {
        return false;
      }

      // Computes ankle correction.
      if (aim_ik_ && !ApplyAnkleAimIK(leg, ray.hit_normal, inv_root)) {
        return false;
      }

      // Updates model-space transformation now ankle local changes is done.
      // Ankle rotation has already been updated, but its siblings (or it's
      // parent siblings) might are not. So we local-to-model update must
      // be complete starting from hip.
      ltm_job.from = leg.hip;
      ltm_job.to = ozz::animation::Skeleton::kMaxJoints;
      if (!ltm_job.Run()) {
        return false;
      }
    }
    return true;
  }

  bool ApplyLegTwoBoneIK(const LegSetup& _leg,
                         const ozz::math::Float3& _target_ws,
                         const ozz::math::Float4x4& _inv_root) {
    // This function will compute two bone IK on the leg, updating hip and knee
    // rotations so that ankle can reach its targetted position.

    // Target position and pole vectors must be in model space.
    const ozz::math::SimdFloat4 target_ms = TransformPoint(
        _inv_root, ozz::math::simd_float4::Load3PtrU(&_target_ws.x));
    const ozz::math::SimdFloat4 pole_vector_ms = models_[_leg.knee].cols[1];

    // Builds two bone iIK job.
    ozz::animation::IKTwoBoneJob ik_job;
    ik_job.target = target_ms;
    ik_job.pole_vector = pole_vector_ms;
    // Mid axis (knee) is constant (usualy), and arbitratry defined by
    // skeleton/rig setup.
    ik_job.mid_axis = ozz::math::simd_float4::z_axis();
    ik_job.weight = weight_;
    ik_job.soften = soften_;
    ik_job.start_joint = &models_[_leg.hip];
    ik_job.mid_joint = &models_[_leg.knee];
    ik_job.end_joint = &models_[_leg.ankle];
    ozz::math::SimdQuaternion start_correction;
    ik_job.start_joint_correction = &start_correction;
    ozz::math::SimdQuaternion mid_correction;
    ik_job.mid_joint_correction = &mid_correction;
    if (!ik_job.Run()) {
      return false;
    }
    // Apply IK quaternions to their respective local-space transforms.
    // Model-space transformations needs to be updated after a call to this
    // function.
    ozz::sample::MultiplySoATransformQuaternion(_leg.hip, start_correction,
                                                make_range(locals_));
    ozz::sample::MultiplySoATransformQuaternion(_leg.knee, mid_correction,
                                                make_range(locals_));

    return true;
  }

  bool ApplyAnkleAimIK(const LegSetup& _leg,
                       const ozz::math::Float3& _target_ws,
                       const ozz::math::Float4x4& _inv_root) {
    // This function will compute aim IK on the ankle, updating its rotations so
    // it can be aligned with the floor.
    // The strategy is to align ankle up vector in the direction of the floor
    // normal. The forward direction of the foot is then driven by the pole
    // vector, which polls the foot (ankle forward vector) toward it's original
    // (animated) direction.

    // Target position and pole vectors must be in model space.
    const ozz::math::SimdFloat4 target_ms = TransformVector(
        _inv_root, ozz::math::simd_float4::Load3PtrU(&_target_ws.x));

    ozz::animation::IKAimJob ik_job;
    // Forward and up vectors are constant (usualy), and arbitratry defined by
    // skeleton/rig setup.
    ik_job.forward = -ozz::math::simd_float4::x_axis();
    ik_job.up = ozz::math::simd_float4::y_axis();

    // Model space targetted direction (floor normal in this case).
    ik_job.target = target_ms;

    // Uses constant ankle Y (skeleton/rig setup dependent) as pole vector. That
    // allows to maintain foot direction.
    ik_job.pole_vector = models_[_leg.ankle].cols[1];

    ik_job.joint = &models_[_leg.ankle];
    ik_job.weight = weight_;
    ozz::math::SimdQuaternion correction;
    ik_job.joint_correction = &correction;
    if (!ik_job.Run()) {
      return false;
    }
    // Apply IK quaternions to their respective local-space transforms.
    // Model-space transformations needs to be updated after a call to this
    // function.
    ozz::sample::MultiplySoATransformQuaternion(_leg.ankle, correction,
                                                make_range(locals_));

    return true;
  }

  virtual bool OnDisplay(ozz::sample::Renderer* _renderer) {
    const float kAxeScale = .1f;
    const ozz::math::Float4x4 kAxesScale =
        ozz::math::Float4x4::Scaling(ozz::math::simd_float4::Load1(kAxeScale));
    const ozz::math::Float4x4 identity = ozz::math::Float4x4::identity();
    const ozz::math::Float4x4 root = GetRootTransform();
    const ozz::math::Float4x4 offseted_root = GetOffsetedRootTransform();

    bool success = true;

    // Renders floor meshes.
    for (size_t i = 0; i < floors_.size(); ++i) {
      success &= _renderer->DrawMesh(floors_[i], identity);
    }

    // Renders character.
    if (show_skin_) {
      // Builds skinning matrices.
      // The mesh might not use (aka be skinned by) all skeleton joints. We
      // use the joint remapping table (available from the mesh object) to
      // reorder model-space matrices and build skinning ones.
      for (size_t m = 0; m < meshes_.size(); ++m) {
        const ozz::sample::Mesh& mesh = meshes_[m];
        for (size_t i = 0; i < mesh.joint_remaps.size(); ++i) {
          skinning_matrices_[i] =
              models_[mesh.joint_remaps[i]] * mesh.inverse_bind_poses[i];
        }

        success &= _renderer->DrawSkinnedMesh(
            mesh, make_range(skinning_matrices_), offseted_root);
      }
    } else {
      // Renders skeleton only.
      success &=
          _renderer->DrawPosture(skeleton_, make_range(models_), offseted_root);
    }

    // Showing joints
    if (show_joints_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const LegSetup& leg = legs_setup_[l];
        for (size_t i = 0; i < 3; ++i) {
          const int joints[3] = {leg.hip, leg.knee, leg.ankle};
          const ozz::math::Float4x4& transform =
              offseted_root * models_[joints[i]];
          success &= _renderer->DrawAxes(transform * kAxesScale);
        }
      }
    }

    // Shows raycast results
    if (show_raycast_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const LegRayInfo& ray = rays_info_[l];
        if (ray.hit) {
          const ozz::sample::Renderer::Color color = {0, 0xff, 0, 0xff};
          success &=
              _renderer->DrawSegment(ray.start, ray.hit_point, color, identity);
          const ozz::sample::Renderer::Color color_n = {0xff, 0, 0, 0xff};
          success &= _renderer->DrawSegment(
              ray.hit_point, ray.hit_point + ray.hit_normal * .5f, color_n,
              identity);
        } else {
          const ozz::sample::Renderer::Color color = {0xff, 0xff, 0xff, 0xff};
          success &= _renderer->DrawSegment(
              ray.start, ray.start + ray.dir * 10.f, color, identity);
        }
      }
    }

    // Shows two bone ik ankle target
    if (show_ankle_target_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const LegRayInfo& ray = rays_info_[l];
        if (ray.hit) {
          const ozz::math::Float4x4& transform =
              ozz::math::Float4x4::Translation(
                  ozz::math::simd_float4::Load3PtrU(&ankles_target_ws_[l].x));
          success &= _renderer->DrawAxes(transform * kAxesScale);
        }
      }
    }

    if (show_root_) {
      success &= _renderer->DrawAxes(root);
    }
    if (show_offseted_root_) {
      success &= _renderer->DrawAxes(offseted_root);
    }

    return success;
  }

  virtual bool OnInitialize() {
    // Reading skeleton.
    if (!ozz::sample::LoadSkeleton(OPTIONS_skeleton, &skeleton_)) {
      return false;
    }

    // Reading animation.
    if (!ozz::sample::LoadAnimation(OPTIONS_animation, &animation_)) {
      return false;
    }

    // Allocates runtime buffers.
    const int num_soa_joints = skeleton_.num_soa_joints();
    locals_.resize(num_soa_joints);
    const int num_joints = skeleton_.num_joints();
    models_.resize(num_joints);

    // Allocates a cache that matches animation requirements.
    cache_.Resize(num_joints);

    // Finds left and right joints.
    const char* left_joints[] = {"LeftUpLeg", "LeftLeg", "LeftFoot"};
    if (!SetupLeg(skeleton_, left_joints, &legs_setup_[kLeft])) {
      return false;
    }
    const char* right_joints[] = {"RightUpLeg", "RightLeg", "RightFoot"};
    if (!SetupLeg(skeleton_, right_joints, &legs_setup_[kRight])) {
      return false;
    }

    // Reading character mesh.
    if (!ozz::sample::LoadMeshes(OPTIONS_mesh, &meshes_)) {
      return false;
    }

    // The number of joints of the mesh needs to match skeleton.
    for (size_t m = 0; m < meshes_.size(); ++m) {
      const ozz::sample::Mesh& mesh = meshes_[m];
      if (num_joints < mesh.highest_joint_index()) {
        ozz::log::Err() << "The provided mesh doesn't match skeleton "
                           "(joint count mismatch)."
                        << std::endl;
        return false;
      }
    }
    skinning_matrices_.resize(num_joints);

    // Reading collision/rendering floor mesh.
    if (!ozz::sample::LoadMeshes(OPTIONS_floor, &floors_)) {
      return false;
    }

    return true;
  }

  bool SetupLeg(const ozz::animation::Skeleton& _skeleton,
                const char* _joint_names[3], LegSetup* _leg) {
    int found = 0;
    int joints[3];
    for (int i = 0; i < _skeleton.num_joints() && found != 3; i++) {
      const char* joint_name = _skeleton.joint_names()[i];
      if (std::strcmp(joint_name, _joint_names[found]) == 0) {
        joints[found] = i;
        ++found;
      }
    }
    _leg->hip = joints[0];
    _leg->knee = joints[1];
    _leg->ankle = joints[2];
    return found == 3;
  }

  virtual void OnDestroy() {}

  virtual bool OnGui(ozz::sample::ImGui* _im_gui) {
    char txt[32];

    // Main options
    {
      _im_gui->DoCheckBox("Auto character height", &auto_character_height_);
      _im_gui->DoCheckBox("Pelvis correction", &pelvis_correction_);
      _im_gui->DoCheckBox("Two bone IK (legs)", &two_bone_ik_);
      _im_gui->DoCheckBox("Aim IK (ankles)", &aim_ik_);
    }

    // Exposes animation runtime playback controls.
    {
      static bool open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Animation control", &open);
      if (open) {
        controller_.OnGui(animation_, _im_gui);
      }
    }

    {  // Root
      static bool opened = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Root transformation", &opened);
      if (opened) {
        // Translation
        _im_gui->DoLabel("Translation");
        sprintf(txt, "x %.2g", root_translation_.x);
        _im_gui->DoSlider(txt, -10.f, 10.f, &root_translation_.x);
        sprintf(txt, "y %.2g", root_translation_.y);
        _im_gui->DoSlider(txt, 0.f, 5.f, &root_translation_.y, 1.f,
                          !auto_character_height_);
        sprintf(txt, "z %.2g", root_translation_.z);
        _im_gui->DoSlider(txt, -10.f, 10.f, &root_translation_.z);

        // Rotation (in euler form)
        _im_gui->DoLabel("Rotation");
        sprintf(txt, "yaw %.3g", root_yaw_ * ozz::math::kRadianToDegree);
        _im_gui->DoSlider(txt, -ozz::math::kPi, ozz::math::kPi, &root_yaw_);
      }
    }

    // Settings
    {
      sprintf(txt, "Foot height %.2g", foot_heigh_);
      _im_gui->DoSlider(txt, 0.f, .3f, &foot_heigh_);
      sprintf(txt, "Weight %.2g", weight_);
      _im_gui->DoSlider(txt, 0.f, 1.f, &weight_);
      sprintf(txt, "Soften %.2g", soften_);
      _im_gui->DoSlider(txt, 0.f, 1.f, &soften_, 1.f, two_bone_ik_);
    }

    // Options
    {
      _im_gui->DoCheckBox("Show skin", &show_skin_);
      _im_gui->DoCheckBox("Show joints", &show_joints_);
      _im_gui->DoCheckBox("Show raycasts", &show_raycast_);
      _im_gui->DoCheckBox("Show ankle target", &show_ankle_target_);
      _im_gui->DoCheckBox("Show root", &show_root_);
      _im_gui->DoCheckBox("Show offseted root", &show_offseted_root_);
    }
    return true;
  }

  virtual void GetSceneBounds(ozz::math::Box* _bound) const {
    ozz::math::Box posture_bound;
    ozz::sample::ComputePostureBounds(make_range(models_), &posture_bound);
    *_bound = TransformBox(GetRootTransform(), posture_bound);
  }

  ozz::math::Float4x4 GetRootTransform() const {
    return ozz::math::Float4x4::Translation(
               ozz::math::simd_float4::Load3PtrU(&root_translation_.x)) *
           ozz::math::Float4x4::FromEuler(
               ozz::math::simd_float4::LoadX(root_yaw_));
  }

  ozz::math::Float4x4 GetOffsetedRootTransform() const {
    if (!pelvis_correction_) {
      return GetRootTransform();
    }

    const ozz::math::Float3 offseted_translation =
        pelvis_offset_ + root_translation_;

    return ozz::math::Float4x4::Translation(
               ozz::math::simd_float4::Load3PtrU(&offseted_translation.x)) *
           ozz::math::Float4x4::FromEuler(
               ozz::math::simd_float4::LoadX(root_yaw_));
  }

 private:
  // Playback animation controller. This is a utility class that helps with
  // controlling animation playback time.
  ozz::sample::PlaybackController controller_;

  // Runtime skeleton.
  ozz::animation::Skeleton skeleton_;

  // Runtime animation.
  ozz::animation::Animation animation_;

  // Sampling cache.
  ozz::animation::SamplingCache cache_;

  // Buffer of local transforms as sampled from animation_.
  ozz::Vector<ozz::math::SoaTransform>::Std locals_;

  // Buffer of model space matrices.
  ozz::Vector<ozz::math::Float4x4>::Std models_;

  // Buffer of skinning matrices, result of the joint multiplication of the
  // inverse bind pose with the model space matrix.
  ozz::Vector<ozz::math::Float4x4>::Std skinning_matrices_;

  // The mesh used by the sample.
  ozz::Vector<ozz::sample::Mesh>::Std meshes_;

  enum { kLeft, kRight };
  enum { kLegsCount = 2 };

  LegSetup legs_setup_[kLegsCount];
  LegRayInfo rays_info_[kLegsCount];
  ozz::math::Float3 ankles_target_ws_[kLegsCount];

  LegRayInfo capsule;

  ozz::math::Float3 pelvis_offset_;

  // The floor meshes used by the sample (collision and rendering).
  ozz::Vector<ozz::sample::Mesh>::Std floors_;

  // Root transformation.
  ozz::math::Float3 root_translation_;
  float root_yaw_;

  // Foot height setting
  float foot_heigh_;

  float weight_;
  float soften_;

  bool auto_character_height_;
  bool pelvis_correction_;
  bool two_bone_ik_;
  bool aim_ik_;

  bool show_skin_;
  bool show_joints_;
  bool show_raycast_;
  bool show_ankle_target_;
  bool show_root_;
  bool show_offseted_root_;
};

int main(int _argc, const char** _argv) {
  const char* title = "Ozz-animation sample: Foot IK";
  return FootIKSampleApplication().Run(_argc, _argv, "1.0", title);
}
