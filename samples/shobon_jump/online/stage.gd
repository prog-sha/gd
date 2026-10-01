## One stage. Syncs the clear display.
extends Node2D

@online var cleared := $Clear.visible # Whether someone reached the right edge
@online var winner := $Clear/By.text  # Text naming who cleared
