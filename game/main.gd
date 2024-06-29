extends Node


func _ready():
	print("Known subclasses of AudioStream:")
	var arr = ClassDB.get_inheriters_from_class("AudioStream")
	arr.sort()
	for clz in arr:
		print(clz)
	
	print("hello world")
