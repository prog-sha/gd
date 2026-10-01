## The Pong world. Holds the court, score, result and player admission.
extends Online

const BALL := preload("res://online/Ball.tscn")
const WIN := 5                                     # First side to reach this score wins

@online($LeftScore.text) var left_score := 0   # Kept as a number and shown on the Label
@online($RightScore.text) var right_score := 0
@online var status := $Status.text
var playing := false                           # Whether a match is running. Stops on a result or while waiting for players

## Only one Ball. It stops itself while nobody plays, so it stays spawned.
@online
func _ready():
	spawn(BALL)

## Current Paddles. A joining player's Paddle is placed by the Secret's player_scene; the Paddle picks its own side, color and name.
func paddles():
	return get_tree().get_nodes_in_group("paddle")

## Start once two players are present.
func _on_player_joining(_my):
	if paddles().size() == 2:
		start()

## Stop the match when fewer than two remain. The leaving player's Paddle is already gone.
func _on_player_left(_my):
	if paddles().size() < 2:
		playing = false
		status = "WAITING FOR 2 PLAYERS"

## Start a match. Rematches also come back here.
func start():
	left_score = 0
	right_score = 0
	for paddle in paddles():
		paddle.result = "YOU"
	status = "PLAY!"
	playing = true

## The Ball left the court; side is the scorer. First to WIN ends the match, shows the result briefly, then rematches.
func goal(side):
	if side < 0.0:
		left_score += 1
	else:
		right_score += 1
	if maxi(left_score, right_score) < WIN:
		return
	playing = false
	status = ("LEFT" if side < 0.0 else "RIGHT") + " WINS!"
	for paddle in paddles():
		paddle.result = "YOU WIN!" if paddle.side == side else "YOU LOSE"
	await get_tree().create_timer(3.0).timeout
	# If a rejoining player already restarted during the wait, don't reset to 0-0 twice
	if not playing and paddles().size() >= 2:
		start()
