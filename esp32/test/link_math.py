Import("env")

# Linux CI requires libm explicitly for native tests that exercise Bayesian
# fusion math helpers. macOS links these symbols implicitly, which hid this.
env.Append(LIBS=["m"])
