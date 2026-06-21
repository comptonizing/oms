class Container():
    def __init__(self, value=None):
        self.__value = value
    def set(self, value):
        self.__value = value
    def get(self):
        return self.__value
