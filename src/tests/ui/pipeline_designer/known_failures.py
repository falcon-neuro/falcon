"""Marks a test that shows a defect the designer still has."""


def known_failure(reason):
    """The test stays and is run, but its failing is expected until the defect is mended. run.py
    reports it apart, and reports its passing as something to look at: the mark is then stale."""
    def mark(test):
        test.known_failure_reason = reason
        return test
    return mark
