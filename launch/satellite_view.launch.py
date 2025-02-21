from launch import LaunchDescription
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory
import os
from launch.substitutions import LaunchConfiguration
from launch.actions import DeclareLaunchArgument

def generate_launch_description():
    rviz_config_path = os.path.join(
        get_package_share_directory('rviz_satellite'),
        'rviz2',
        'map.rviz'
    )

    gps_frame = LaunchConfiguration('gps_frame', default='gps')

    return LaunchDescription([
        DeclareLaunchArgument(
            'gps_frame',
            default_value='gps',
            description='Frame ID for the GPS data'
        ),
        Node(
            package='rviz2',
            executable='rviz2',
            name='rviz2',
            output='screen',
            arguments=['-d', rviz_config_path]
        ),
        # Node(
        #     package='tf2_ros',
        #     executable='static_transform_publisher',
        #     name='static_transform_publisher',
        #     output='screen',
        #     arguments=['0', '0', '0', '0', '0', '0', 'map', gps_frame]
        # )
    ])