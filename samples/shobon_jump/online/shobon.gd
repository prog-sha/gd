## Player input plus the Online-side processing that moves that player.
extends CharacterBody2D

const GRAVITY := 1500.0     # Fall acceleration per second
const FALL_MAX := 900.0     # Maximum fall speed
const RUN_SPEED := 220.0    # Maximum horizontal speed
const RUN_ACCEL := 1800.0   # Horizontal speed change rate
const JUMP_SPEED := 515.0   # Upward jump speed. Rises about 76px at the world tick rate (20/s)
const TACKLE_SEC := 0.24    # Tackle duration in seconds
const TACKLE_COOL := 0.75   # Tackle cooldown in seconds
const TACKLE_SPEED := 330.0 # Dash speed, slightly faster than running
const TACKLE_X := 74.0      # Forward width in which teammates get launched
const TACKLE_Y := 58.0      # Forward height in which teammates get launched
const LAUNCH_X := 620.0     # Horizontal launch speed for teammates
const LAUNCH_Y := 620.0     # Upward launch speed for teammates
const FINISH_X := 1080.0    # Flag x position. Reaching it clears the stage for everyone
const COLORS := [Color("fff3c4"), Color("c8f2ff"), Color("ffd2eb")] # Colors by join order

@online var display_name := $Name.text
@online var tone := $Face.modulate
@online var charging := $Burst.visible # Tackle visual
@online_save var clears := 0           # Times the flag was reached, kept across restarts
@online_input var axis := 0.0          # Player's left/right input. Written by the client, read by the Online side
var facing := 1.0
var tackle_left := 0.0                 # Remaining tackle seconds
var tackle_cool := 0.0                 # Remaining cooldown seconds
var launched_left := 0.0               # Remaining seconds to keep the speed from a teammate's launch
var home := Vector2.ZERO               # Start point to return to after falling

## After setup, the host decides this player's look and start point.
@online
func _ready():
	world.joined += 1
	var order = world.joined
	display_name = "（´・ω・`） %d" % order
	tone = COLORS[order % COLORS.size()]
	position.x = 190.0 - (order - 1) * 60.0
	home = position

## Runs only on the player's own client. Shows the own-player marker.
@online_my
func _ready():
	$Mine.visible = true

## Runs on the player's client. Left/right is sent by writing it; jump and tackle are requested as events.
func _process(_delta):
	axis = Input.get_axis("ui_left", "ui_right")
	if Input.is_action_just_pressed("ui_accept"):
		jump()
	if Input.is_action_just_pressed("tackle"):
		tackle()

@online
func jump():
	if is_on_floor():
		velocity.y = -JUMP_SPEED

## Start a dash if the cooldown has expired.
@online
func tackle():
	if tackle_cool <= 0.0:
		tackle_left = TACKLE_SEC
		tackle_cool = TACKLE_COOL

## Advance movement, tackle and goal in order.
@online
func _physics_process(delta):
	tackle_cool = maxf(0.0, tackle_cool - delta)
	tackle_left = maxf(0.0, tackle_left - delta)
	launched_left = maxf(0.0, launched_left - delta)
	if absf(axis) > 0.01:
		facing = signf(axis)
	velocity.y = minf(FALL_MAX, velocity.y + GRAVITY * delta)
	if tackle_left > 0.0:
		velocity.x = facing * TACKLE_SPEED
		charging = true
	else:
		if launched_left <= 0.0:
			velocity.x = move_toward(velocity.x, clampf(axis, -1.0, 1.0) * RUN_SPEED, RUN_ACCEL * delta)
		charging = false
	move_and_slide()
	if position.y > 720.0:
		restart()
	var stage = world.stage
	# Launch each teammate in front once while tackling
	if charging:
		for target in get_tree().get_nodes_in_group("shobon"):
			if target == self or target.launched_left > 0.0:
				continue
			var offset = target.position - position
			if offset.x * facing < -8.0 or absf(offset.x) > TACKLE_X \
					or absf(offset.y) > TACKLE_Y:
				continue
			target.velocity = Vector2(facing * LAUNCH_X, -LAUNCH_Y)
			target.launched_left = TACKLE_SEC
	# The first player to reach the flag clears the stage for everyone
	if not stage.cleared and position.x >= FINISH_X:
		clears += 1
		world.clear_stage("%s  CLEAR! (%d回目)" % [display_name, clears])

## Return to the start point after a fall or when the next stage opens.
func restart():
	position = home
	velocity = Vector2.ZERO
