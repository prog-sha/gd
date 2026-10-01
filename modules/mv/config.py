# Build configuration for the mv module: built only into the view executable, with an option for the private Secret authority.


def can_build(env, platform):
    return env["view"]  # The node and scene tree hooks exist only in the view sources.


def get_opts(platform):
    from SCons.Variables import BoolVariable

    return [
        BoolVariable("online_secret_enabled", "Build the private Secret authority", False),
    ]


def configure(env):
    pass
