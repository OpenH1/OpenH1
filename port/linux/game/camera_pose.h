/*
CAMERA_POSE.H

The camera put at a place and facing from the console (camera_pose.c).
*/

#ifndef CAMERA_POSE_H
#define CAMERA_POSE_H

/* "<x> <y> <z> <yaw> <pitch> <roll> <field of view>": world units, and
degrees (the field of view the game's own, across at 4:3); the script's
camera is taken and put there at once. "off" gives the camera back */
void camera_pose_set(char const *pose);

#endif
