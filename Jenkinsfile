node {
    stage("Checkout") {
        git branch: 'master' 'https://github.com/c0r73x/lsext.git'
    }
    stage("Build") {
        sh "make"
    }
}
