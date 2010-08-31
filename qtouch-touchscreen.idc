# Input Device Calibration File

# Touch Area
touch.touchArea.calibration = pressure

# Tool Area
#   Raw width field measures approx. 1 unit per millimeter
#   of tool area on the surface where a raw width of 14 corresponds
#   to about 18mm of physical size.  Given that the display resolution
#   is 6px per mm we obtain a scale factor of 6 pixels / unit and
#   a bias of 24 pixels.  Each contact point is measured independently
#   so the raw width is not a sum.
#
#   The linear approximation isn't particularly good here.  We
#   might want to use a second order polynomial fit instead since the width
#   appears to scale with touch area as opposed to with touch diameter.
#
#   Calibration measurements:
#     13mm -> raw width = 7-8
#     14mm -> raw width = 8-9
#     17mm -> raw width = 12-13
#     18mm -> raw width = 14-15
#     19mm -> raw width = 15-16
#     21mm -> raw width = 17-19
#     24mm -> raw width = 23-26
touch.toolArea.calibration = linear
touch.toolArea.linearScale = 6
touch.toolArea.linearBias = 24
touch.toolArea.isSummed = 0

# Pressure
#   Driver reports signal strength as pressure.
#   A normal thumb touch while touching the back of the device
#   typically registers about 80 signal strength units although
#   this value is highly variable and is sensitive to contact area,
#   manner of contact and environmental conditions.  We set the
#   scale so that a normal touch with good signal strength will be
#   reported as having a pressure somewhere in the vicinity of 1.0,
#   a featherlight touch will be below 1.0 and a heavy or large touch
#   will be above 1.0.  We don't expect these values to be accurate.
touch.pressure.calibration = amplitude
touch.pressure.source = default
touch.pressure.scale = 0.0125

# Size
touch.size.calibration = normalized

# Orientation
touch.orientation.calibration = none

