## Player's up/down input and Paddle movement on the Online side.
extends Node2D

const SPEED := 440.0
const COLORS := [Color("42d9ff"), Color("ff4ca1")] # Left, right

@online var display_name := $Name.text
@online var tone := $Body.color
@online_my var result := $Mine.text # Marker and result shown only to the player
@online_input var axis := 0.0       # Player's up/down input. Written by the client, read by the Online side
var side := -1.0                    # -1 for left, 1 for right

## After setup, the host picks the open side and the slot on it.
@online
func _ready():
	var others = world.paddles().filter(func(one): return one != self)
	var lefts = others.filter(func(one): return one.side < 0.0).size()
	var left = lefts * 2 <= others.size()
	side = -1.0 if left else 1.0
	position = Vector2(576.0 + side * 506.0,
		356.0 + ((lefts if left else others.size() - lefts) * 2 - 1) * 90.0)
	display_name = "LEFT" if left else "RIGHT"
	tone = COLORS[0 if left else 1]
	result = "YOU"

## Runs on the player's client. Writing the input is enough to reach the Online side.
func _process(_delta):
	axis = Input.get_axis("ui_up", "ui_down")

## Move the Paddle on the Online side and keep it on the court. Input is only the player's claim, so its trusted range is decided here.
@online
func _physics_process(delta):
	position.y = clampf(position.y + clampf(axis, -1.0, 1.0) * SPEED * delta, 160.0, 552.0)
