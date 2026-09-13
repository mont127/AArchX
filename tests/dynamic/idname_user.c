int shared_value(void);

int user_value(void)
{
    return shared_value() + 1;
}
