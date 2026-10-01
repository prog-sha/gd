## Manages only player joins and Stage progress on the Online side.
extends Online

const STAGES := [preload("res://online/Stage1.tscn"), preload("res://online/Stage2.tscn")] # Opened in order

var stage       # Current stage
var joined := 0 # Players joined so far, counted by Shobon. Never decreases, so numbers never collide
var level := 0

## Start the world with the first stage.
@online
func _ready():
	stage = spawn(STAGES[level])

## Someone reached the flag. Show it briefly, open the next stage and send everyone back to the start.
func clear_stage(by):
	stage.winner = by
	stage.cleared = true
	await get_tree().create_timer(2.0).timeout
	level = (level + 1) % STAGES.size()
	stage.queue_free()
	stage = spawn(STAGES[level])
	for shobon in get_tree().get_nodes_in_group("shobon"):
		shobon.restart()
