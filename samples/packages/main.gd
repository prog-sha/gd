# Import an installed registry package through its local alias.
@import hello

# Print a greeting supplied by the package.
func main():
	print(hello.message("world"))
	return 0
