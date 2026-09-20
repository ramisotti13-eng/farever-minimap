#pragma once

namespace farever {

// Pure selection of the compass strip's reference heading (#65). When the
// "follow camera" setting is on AND a valid camera yaw is available, the strip
// recenters on the camera yaw (smooth, matches the screen); otherwise it falls
// back to the hero's facing. All angles in radians. No normalisation: the
// strip's project() already wraps the relative bearing into [-pi, pi], so an
// unbounded camera yaw is fine to return verbatim.
inline float compass_heading_ref(bool follow_camera, bool camera_valid,
                                 double camera_yaw, double hero_rot_z) {
    if (follow_camera && camera_valid) return static_cast<float>(camera_yaw);
    return static_cast<float>(hero_rot_z);
}

}  // namespace farever
