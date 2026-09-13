int rpath_dep_value(void);

int rpath_user_value(void)
{
    return rpath_dep_value() + 1;
}
