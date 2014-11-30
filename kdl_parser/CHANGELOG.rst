^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package kdl_parser
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1.10.21 (2014-11-30)
--------------------

1.10.20 (2014-08-01)
--------------------
* add version dependency on orocos_kdl >= 1.3.0
* kdl_parser: Adding kdl library explicitly so that dependees can find it
* Update KDL SegmentMap interface to optionally use shared pointers
  The KDL Tree API optionally uses shared pointers on platforms where
  the STL containers don't support incomplete types.
* Contributors: Brian Jensen, Jonathan Bohren, William Woodall

1.10.19 (2014-02-15)
-------------------
* fix test at kdl_parser
* Contributors: YoheiKakiuchi

1.10.18 (2013-12-04)
--------------------
* add DEPENDS for kdl_parser
* Contributors: Ioan Sucan

1.10.16 (2013-11-18)
--------------------
* check for CATKIN_ENABLE_TESTING

1.10.15 (2013-08-17)
--------------------
* fix `#30 <https://github.com/ros/robot_model/issues/30>`_
