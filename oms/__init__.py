import os
from flask import Flask
from .api.v1 import api_v1_page

def create_app():
    app = Flask(__name__)
    app.register_blueprint(api_v1_page)

    @app.route('/hello')
    def hello():
        return "Hello"

    return app
