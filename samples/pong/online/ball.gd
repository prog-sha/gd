## Ball movement and bouncing. The Online side advances it; position is distributed without extra code.
extends Node2D

const TOP := 101.0    # Top limit of the Ball center
const BOTTOM := 607.0 # Bottom limit of the Ball center
const HIT_X := 25.0   # Horizontal hit range against a Paddle
const HIT_Y := 73.0   # Vertical hit range against a Paddle

var motion := Vector2.ZERO    # Direction and speed. Starts from the initial velocity set in the Scene
@onready var home := position # Center placed in the Scene. Reset here on every Goal
@onready var launch := motion # Launch direction. Flips left/right on every Goal

## Advance on the Online side and bounce off walls, Paddles and Goals.
@online
func _physics_process(delta):
	if not world.playing:
		position = home
		motion = Vector2.ZERO
		return
	if motion == Vector2.ZERO:
		motion = launch
	position += motion * delta
	if position.y <= TOP or position.y >= BOTTOM:
		position.y = clampf(position.y, TOP, BOTTOM)
		motion.y = -motion.y
	for paddle in world.paddles():
		if motion.x * paddle.side > 0.0 \
				and absf(position.x - paddle.position.x) <= HIT_X \
				and absf(position.y - paddle.position.y) <= HIT_Y:
			position.x = paddle.position.x - paddle.side * HIT_X
			motion = Vector2(-motion.x, clampf(motion.y + (position.y - paddle.position.y) * 4.0, -430.0, 430.0))
	if position.x < -20.0 or position.x > 1172.0:
		world.goal(-1.0 if position.x > 1172.0 else 1.0)
		launch.x = -launch.x
		position = home
		motion = launch
