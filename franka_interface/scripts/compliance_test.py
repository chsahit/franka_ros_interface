import rospy
from franka_interface import ArmInterface, GripperInterface

if __name__ == '__main__':
    rospy.init_node("compliant_motions_node")
    arm = ArmInterface()
    gripper = GripperInterface()
    gripper.close()


