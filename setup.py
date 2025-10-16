import sys
from setuptools import setup, Extension
from Cython.Build import cythonize
import numpy as np

if sys.platform == 'win32':
    # Pour le compilateur MSVC de Windows
    compile_args = [
        '/openmp',
        '/DNPY_NO_DEPRECATED_API=NPY_1_7_API_VERSION',  # Supprime le warning NumPy
        '/wd4244',  # Supprime les warnings de conversion Py_ssize_t -> long
        '/wd4551',  # Supprime le warning "function call missing argument list"
    ]
    link_args = []  # MSVC n'a pas besoin de flag de liaison pour OpenMP
else:
    # Pour les compilateurs GCC/Clang sur Linux et macOS
    compile_args = ['-fopenmp', '-DNPY_NO_DEPRECATED_API=NPY_1_7_API_VERSION']
    link_args = ['-fopenmp']

extensions = [
    Extension(
        name="MCMM_client",
        sources=["MCMM_client.pyx"],
        include_dirs=[np.get_include()],
        language="c++",
        extra_compile_args=compile_args,
        extra_link_args=link_args,
    )
]

setup(
    name="MCMM_client",
    version="1.0",
    description="Minecraft Video to Map Converter",
    ext_modules=cythonize(
        extensions,
        compiler_directives={
            'language_level': "3",
            'boundscheck': False,
            'wraparound': False,
            'nonecheck': False,
            'cdivision': True,
        },
        annotate=True  # Génère un fichier HTML pour voir l'optimisation
    ),
    zip_safe=False,
)